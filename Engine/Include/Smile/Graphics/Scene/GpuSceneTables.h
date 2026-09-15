#pragma once

// Tabelas CANONICAS residentes na GPU (Fase 2 do GPU-DRIVEN-PLAN).
//
// Tres: materiais, meshes e instancias. A tabela de instancias nasce na Fase 2 como fonte
// canonica; as fases seguintes migram os consumidores raster/indirect sem mudar sua ABI.
//
// O que a de MATERIAIS resolve, em uma frase: editar um material passa a tocar UMA linha de 116 B
// em vez de reescrever o snapshot inteiro da cena (2.479 registros de 100 B na Emerald) com as
// duas filas drenadas.
//
// O que a de MESHES resolve e diferente, e vale ser honesto: ela NAO economiza byte nenhum no
// snapshot por instancia — VB/IB saem, MeshSrv/MeshId entram. O ganho e ter uma casa unica para
// bounds locais, contagens e os descritores bindless de geometria, que os consumidores raster e o
// culling das fases seguintes vao ler. E ela toma a posse dos descritores de geometria, que
// moravam no FRaytracingScene: decisao D12 do plano — o ray tracing e consumidor da cena, nao dono
// dela. Junto vem a enumeracao das malhas unicas, que passa a existir num lugar so (o
// FRaytracingScene le a lista daqui para dimensionar os BLAS).
//
// Tres decisoes que nao sao obvias:
//
//  1. DEFAULT heap, nao upload. O snapshot por instancia mora em upload heap — memoria do host,
//     lida por PCIe em placa discreta — e e por isso que reescreve-lo exigia drenar: escrever com
//     frames em voo corrompe o que eles leem. As tabelas vivem em VRAM e recebem as linhas sujas
//     por CopyBufferRegion na command list do frame, entao o versionamento e o da FILA e nao
//     precisa de Flush nenhum.
//
//  2. Uma linha e SUJA por CONTEUDO, nao por aviso. O editor sinaliza "algum material mudou" sem
//     dizer qual (MaterialsBridge chama MarkMaterialRTStateDirty sem argumento), e o sidecar
//     reaplica dezenas de materiais de uma vez. Recalcular todas as linhas e comparar custa
//     ~14 KB de memcmp nas cenas atuais e devolve exatamente o conjunto que mudou — que e o que o
//     gate da fase exige provar.
//
//  3. Os descritores das tabelas viajam DENTRO do snapshot por instancia (MaterialSrv, MeshSrv), e
//     nao por um cbuffer. Assim os treze shaders que ja leem InstanceGeo alcancam as tabelas sem
//     root signature nova: eles indexam o heap direto, como ja fazem para VB/IB/payload. Custa 4 B
//     por instancia por tabela — 10 KB na Emerald, contra a root signature de treze passes.
//     Descritores SEPARADOS, e nao um range contiguo com um indice so: assim redimensionar uma
//     tabela nao obriga a reescrever o descritor da outra.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Scene/GpuMaterialRecord.h"
#include "Smile/Math/Math.h"
#include "Smile/Scene/CapturedIdentity.h"

#include <cstdint>
#include <d3d12.h>
#include <span>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;
    class FGpuMesh;
    class FScene;

    // === Linha de mesh ==========================================================================
    //
    // Espelhada em HLSL como `GpuMeshRecord` (Shaders/Scene/GpuSceneTables.hlsli); a ABI e
    // verificada em WARP.
    struct FGpuMeshRecord {
        // Bounds em espaco do OBJETO, como o cozido v7+ os grava. Sao propriedade da MALHA, e hoje
        // vivem duplicados em cada FRenderable que a usa (FRenderable::LocalAABBMin/Max, copiados
        // do SMeshEntry no load). Aqui e a casa certa; a duplicacao no renderavel sai na Fase 4,
        // quando os consumidores deixam de consulta-lo diretamente.
        Vec3 LocalAABBMin{ 0.0f, 0.0f, 0.0f };
        Vec3 LocalAABBMax{ 0.0f, 0.0f, 0.0f };

        // Descritores bindless da geometria. TRES por malha unica desde a v8 do cozido:
        // [VB][IB][RTTri]. O terceiro e o payload pre-cozido por triangulo, que o caminho de hit
        // le no lugar da cadeia IB -> 3 vertices.
        u32 VertexSrv   = 0;
        u32 IndexSrv    = 0;
        u32 TriangleSrv = 0;

        u32 VertexCount   = 0;
        u32 IndexCount    = 0;
        u32 TriangleCount = 0; // payload de RT; 0 = malha sem payload

        // Ver EGpuMeshFlags. Era RESERVADO e zero em toda malha; a Fase 5 abriu o primeiro bit, e o
        // resto continua reservado para a trilha de virtual geometry (Fase 9+). NAO e serializado no
        // formato cozido: e derivado da malha em runtime.
        u32 Flags             = 0;
        u32 VirtualGeometryId = 0;

        // === Endereco DENTRO do pool de geometria (Fase 5) ======================================
        //
        // Malhas nao tem buffer proprio: elas sao fatias de um pool de ate 256 MB
        // ([VB][IB][RTTri]), e estes dois sao o que o `ExecuteIndirect` precisa para desenhar uma
        // delas com o VB/IB do POOL amarrado uma vez. Os valores ja existiam no FGpuMesh
        // (`VertexFirstElement`/`IndexFirstElement`, usados nos SRVs bindless); o que faltava era
        // publica-los na linha para a GPU poder montar o argumento.
        //
        // Os TIPOS espelham o destino, e nao a origem: `BaseVertexLocation` e INT no contrato do
        // D3D12 e `StartIndexLocation` e UINT. Publicar os dois como u32 funcionaria ate um offset
        // passar de INT_MAX, e ai o comportamento seria silenciosamente outro. Quem garante que o
        // valor cabe e `GpuMeshOffsetsRepresentable`, e quem NAO cabe leva a flag e sai do caminho
        // indireto — nao do raster.
        i32 VertexOffset = 0; // -> BaseVertexLocation, em VERTICES a partir da base do pool
        u32 IndexOffset  = 0; // -> StartIndexLocation, em INDICES a partir da base do pool

        // Qual PAR VB/IB. Um `ExecuteIndirect` amarra um par, entao comandos de pares diferentes
        // nao podem entrar no mesmo lote: e por isso que o id vive na linha, e nao so numa tabela
        // da CPU. Denso e estavel dentro de um setup (ver FGpuMeshTable::GeometryPools).
        //
        // ⚠️ O PAR, e nao o vertex buffer. Os dois caminhos de criacao de malha sao diferentes:
        // `AddMeshesBatch` fatia um pool unico (VB == IB == o chunk), e `AddMesh` — usado pela cena
        // default e pelas primitivas do editor — cria DOIS recursos por malha. Identificar o pool
        // pelo VB sozinho juntaria malhas que nao compartilham index buffer, e o lote amarraria o IB
        // de outra.
        u32 GeometryPoolId = 0;
    };
    static_assert(sizeof(FGpuMeshRecord) == 68, "FGpuMeshRecord deve casar com o HLSL (68 B)");
    static_assert(offsetof(FGpuMeshRecord, LocalAABBMin)      ==  0);
    static_assert(offsetof(FGpuMeshRecord, LocalAABBMax)      == 12);
    static_assert(offsetof(FGpuMeshRecord, VertexSrv)         == 24);
    static_assert(offsetof(FGpuMeshRecord, IndexSrv)          == 28);
    static_assert(offsetof(FGpuMeshRecord, TriangleSrv)       == 32);
    static_assert(offsetof(FGpuMeshRecord, VertexCount)       == 36);
    static_assert(offsetof(FGpuMeshRecord, IndexCount)        == 40);
    static_assert(offsetof(FGpuMeshRecord, TriangleCount)     == 44);
    static_assert(offsetof(FGpuMeshRecord, Flags)             == 48);
    static_assert(offsetof(FGpuMeshRecord, VirtualGeometryId) == 52);
    static_assert(offsetof(FGpuMeshRecord, VertexOffset)      == 56);
    static_assert(offsetof(FGpuMeshRecord, IndexOffset)       == 60);
    static_assert(offsetof(FGpuMeshRecord, GeometryPoolId)    == 64);

    enum EGpuMeshFlags : u32 {
        // O offset da malha no pool nao cabe no contrato do argumento indireto. A linha continua
        // valida para todo o resto (RT, bindless, contagens); o que ela perde e o caminho
        // INDIRETO, e quem a desenha e o fallback CPU. Regra 6 do plano: desenhar pela CPU e
        // aceitavel, geometria sumir nao e.
        GpuMeshOffsetsUnrepresentable = 1u << 0,
        // O `IndexCount` nao descreve triangulos inteiros. Mesma politica: sai do indireto, fica no
        // raster. Ver `GpuMeshIndexCountUsable` para o porque.
        GpuMeshIndexCountUnusable = 1u << 1,
    };

    // Tudo que impede o caminho indireto, numa mascara so.
    //
    // Existe para que um bit novo entre em UM lugar. A alternativa — cada consumidor listando os
    // bits que conhece — ja tem nome: o dia em que o terceiro bit nascer, o consumidor que nao for
    // atualizado emite comando para uma linha que os outros recusam.
    inline constexpr u32 kGpuMeshIndirectBlocked =
        GpuMeshOffsetsUnrepresentable | GpuMeshIndexCountUnusable;

    // O `IndexCount` descreve triangulos INTEIROS?
    //
    // TRIANGLELIST descarta primitiva incompleta em SILENCIO. Cinco indices desenham um triangulo e
    // somem com os outros dois: sem erro de API, sem warning, sem nada — so um pedaco de malha que
    // nao aparece. E zero nao descreve triangulo nenhum.
    //
    // ⚠️ Isto tem de ser decidido na CLASSIFICACAO, e nao no scatter. A contagem por lote RESERVA a
    // posicao do comando; se o scatter rejeitasse depois, `DrawCounts` diria N e existiriam N-1
    // argumentos escritos, e o `ExecuteIndirect` executaria a posicao restante com o comando do
    // frame passado. Por isso a recusa vira FLAG na linha, que o `GpuIndirectEligible` ja consulta:
    // contagem e emissao passam a olhar o mesmo bit.
    inline bool GpuMeshIndexCountUsable(u32 _IndexCount) {
        return _IndexCount >= 3u && (_IndexCount % 3u) == 0u;
    }

    // Preenche `IndexCount` e marca a linha quando ele nao serve ao indireto.
    //
    // Pura e separada pelo mesmo motivo das outras: o ramo invalido exige uma malha com contagem
    // quebrada, que nenhuma cena cozida vai fornecer sob demanda. Aqui basta um inteiro.
    //
    // A flag e LIMPA no ramo valido, pela mesma razao do `GpuMeshFillPoolAddress`: `true` significa
    // "utilizavel pelo caminho indireto", e uma marca de uma chamada anterior contradiria o retorno.
    // So este bit — os outros tem donos diferentes.
    inline bool GpuMeshFillIndexCount(FGpuMeshRecord& _Row, u32 _IndexCount) {
        _Row.IndexCount = _IndexCount;
        if (GpuMeshIndexCountUsable(_IndexCount)) {
            _Row.Flags &= ~static_cast<u32>(GpuMeshIndexCountUnusable);
            return true;
        }
        _Row.Flags |= GpuMeshIndexCountUnusable;
        return false;
    }

    // Offsets POISON da linha irrepresentavel.
    //
    // A primeira versao deixava os dois em ZERO, com o argumento de que "uma leitura que ignore a
    // flag desenha a primeira malha do pool". Isso estava errado: o pool e
    // `[todos os VBs][todos os IBs]`, entao offset de INDICE zero aponta para a regiao de VERTICE, e
    // o command processor leria bytes de vertice como indices. Nao e um desenho errado — e uma
    // leitura de indices arbitrarios.
    //
    // Poison, entao: se um comando for emitido para esta linha, que ele seja absurdo em vez de
    // plausivel. Quem de fato impede a emissao e a FLAG, e provar isso e obrigacao da slice que
    // escrever o scatter.
    inline constexpr i32 kGpuMeshPoisonVertexOffset = INT32_MIN;
    inline constexpr u32 kGpuMeshPoisonIndexOffset  = 0xFFFFFFFFu;

    // Um par VB/IB. E a identidade que um lote de `ExecuteIndirect` pode amarrar de uma vez.
    struct FGpuGeometryPool {
        ID3D12Resource* Vertex = nullptr;
        ID3D12Resource* Index  = nullptr;
    };
    inline bool operator==(const FGpuGeometryPool& A, const FGpuGeometryPool& B) {
        return A.Vertex == B.Vertex && A.Index == B.Index;
    }

    // Cabe no argumento indireto?
    //
    // PURA de proposito: os limites sao alcancaveis com dois inteiros, e nao com um pool de 68 GB.
    // Um teste que precisasse do pool nao existiria, e a checagem viveria so no comentario.
    //
    // Hoje o teto NAO e alcancavel: o orcamento de chunk e 256 MB, o que da 8M vertices e 64M
    // indices por pool. A checagem existe porque esse orcamento e um numero escolhido, e o dia em
    // que ele mudar ninguem vai lembrar deste campo.
    inline bool GpuMeshOffsetsRepresentable(u32 _VertexFirstElement, u32 _IndexFirstElement) {
        // O indice e UINT no contrato, entao qualquer u32 serve; o vertice e INT.
        (void)_IndexFirstElement;
        return _VertexFirstElement <= static_cast<u32>(INT32_MAX);
    }

    // Id DENSO do par, atribuido na primeira aparicao.
    //
    // Pura e separada da tabela pelo mesmo motivo do predicado acima: exercitar a atribuicao pela
    // `SetupForScene` exigiria uma cena com geometria em dois chunks de 256 MB, que nenhum teste vai
    // montar. Aqui bastam quatro ponteiros.
    //
    // Os ponteiros sao COMPARADOS e guardados, nunca dereferenciados — e por isso o teste pode
    // passar valores que nao apontam para recurso nenhum.
    //
    // Busca linear porque o numero de pares e o numero de chunks da cena cozida mais as malhas
    // criadas uma a uma (primitivas do editor, preview de material): unidades, nao milhares.
    // Preenche os campos de endereco da linha, ou a envenena e marca.
    //
    // Pura pelo mesmo motivo das outras duas: dentro do laco que varre `FGpuMesh` o ramo do poison
    // seria inalcancavel por teste — exigiria um pool de 68 GB. Aqui basta um inteiro.
    //
    // Devolve true quando a linha ficou utilizavel pelo caminho indireto.
    inline bool GpuMeshFillPoolAddress(FGpuMeshRecord& _Row, u32 _VertexFirstElement,
                                       u32 _IndexFirstElement) {
        if (GpuMeshOffsetsRepresentable(_VertexFirstElement, _IndexFirstElement)) {
            _Row.VertexOffset = static_cast<i32>(_VertexFirstElement);
            _Row.IndexOffset  = _IndexFirstElement;
            // LIMPA a flag: `true` diz que a linha esta utilizavel pelo caminho indireto, e uma
            // marca de uma chamada anterior a mandaria para o fallback contradizendo o retorno. A
            // transicao invalido -> valido acontece de verdade num resize de cena, em que a malha
            // muda de posicao no pool.
            //
            // So ESTE bit, e nao `Flags = 0`: os outros sao reservados para a trilha de virtual
            // geometry, e esta funcao nao tem nada a dizer sobre eles.
            _Row.Flags &= ~static_cast<u32>(GpuMeshOffsetsUnrepresentable);
            return true;
        }
        _Row.Flags |= GpuMeshOffsetsUnrepresentable;
        _Row.VertexOffset = kGpuMeshPoisonVertexOffset;
        _Row.IndexOffset  = kGpuMeshPoisonIndexOffset;
        return false;
    }

    inline u32 GpuGeometryPoolIdFor(std::vector<FGpuGeometryPool>& _Pools,
                                    const FGpuGeometryPool& _Pool) {
        for (u32 Id = 0; Id < _Pools.size(); ++Id)
            if (_Pools[Id] == _Pool) return Id;
        _Pools.push_back(_Pool);
        return static_cast<u32>(_Pools.size() - 1);
    }

    // === A porta do caminho indireto ============================================================
    //
    // As duas condicoes, num predicado so: a classe do material tem de ser elegivel (Fase 5: apenas
    // `OpaqueOneSided`) e a linha da malha nao pode carregar nenhum bit de `kGpuMeshIndirectBlocked`
    // — hoje offset irrepresentavel e `IndexCount` que nao descreve triangulos inteiros.
    //
    // Junto, e nao duas checagens espalhadas, porque quem emite o comando e quem CONTA o fallback
    // tem de usar a mesma resposta. Duas copias da condicao dariam, no dia em que uma delas
    // esquecesse um termo, um objeto desenhado duas vezes (aceitavel) ou nenhuma (nao e) — e o
    // contador diria que esta tudo certo.
    //
    // ⚠️ E por isso que o termo da malha e uma MASCARA e nao uma lista de bits: a contagem por lote
    // reserva a posicao do comando, entao o classify e o scatter tem de recusar exatamente o mesmo
    // conjunto. Um bit que um conhecesse e o outro nao deixaria uma posicao reservada sem argumento
    // escrito, e o `ExecuteIndirect` executaria ali o comando do frame passado.
    //
    // ⚠️ O que este predicado prova e o que ele NAO prova. Ele e a unica definicao de elegibilidade,
    // e o teste cobre as combinacoes de (classe, flags). Que o EMISSOR de comandos o consulte — ou
    // seja, que a linha bloqueada de fato nao receba argumento — e obrigacao da slice do scatter.
    inline bool GpuIndirectEligible(u32 _PipelineClass, u32 _MeshFlags) {
        return GpuPipelineClassIndirectEligible(_PipelineClass) &&
               (_MeshFlags & kGpuMeshIndirectBlocked) == 0u;
    }

    inline bool operator==(const FGpuMeshRecord& A, const FGpuMeshRecord& B) {
        auto Same3 = [](const Vec3& L, const Vec3& R) {
            return L.X == R.X && L.Y == R.Y && L.Z == R.Z;
        };
        return Same3(A.LocalAABBMin, B.LocalAABBMin) && Same3(A.LocalAABBMax, B.LocalAABBMax) &&
               A.VertexSrv == B.VertexSrv && A.IndexSrv == B.IndexSrv &&
               A.TriangleSrv == B.TriangleSrv && A.VertexCount == B.VertexCount &&
               A.IndexCount == B.IndexCount && A.TriangleCount == B.TriangleCount &&
               A.Flags == B.Flags && A.VirtualGeometryId == B.VirtualGeometryId &&
               // Os tres da Fase 5 tambem, porque igualdade que ignora campo e igualdade errada.
               //
               // Sendo exato sobre a CONSEQUENCIA, que eu havia exagerado: hoje nao ha como isto
               // causar dano, porque o `SetupForScene` limpa o `Shadow_` e um setup ou resize
               // republica a tabela inteira de qualquer jeito. O dano aparece no dia em que a tabela
               // de malha ganhar atualizacao INCREMENTAL — como a de instancias ja tem —, e ai uma
               // linha cuja unica mudanca fosse o offset no pool ficaria para tras em silencio.
               A.VertexOffset == B.VertexOffset && A.IndexOffset == B.IndexOffset &&
               A.GeometryPoolId == B.GeometryPoolId;
    }
    inline bool operator!=(const FGpuMeshRecord& A, const FGpuMeshRecord& B) { return !(A == B); }

    // === Colunas de instancia ==================================================================
    //
    // SoA por SLOT DE RESIDENCIA, nao pela posicao autoral na RenderableList. O slot torna a linha
    // estavel quando a lista compacta; Generation impede que um leitor aceite o ocupante seguinte.
    enum EGpuSceneInstanceFlags : u32 {
        GpuSceneInstanceVisible        = 1u << 0,
        GpuSceneInstanceRaytracingOnly = 1u << 1,
        GpuSceneInstanceDynamic        = 1u << 2,
        // Current e Previous ainda divergem. O bit habilita o caminho temporal caro somente
        // enquanto o objeto realmente se move; o assentamento o limpa junto de Previous.
        GpuSceneInstanceTransformChanged = 1u << 3,
    };

    struct FGpuSceneInstanceMeta {
        u32 MeshId     = 0;
        u32 MaterialId = 0;
        u32 CellId     = 0; // reservado para a grade/ranges das fases seguintes
        u32 Flags      = 0;
        u32 Generation = 0; // 0 = linha vazia
    };
    static_assert(sizeof(FGpuSceneInstanceMeta) == 20);
    static_assert(offsetof(FGpuSceneInstanceMeta, MeshId)     ==  0);
    static_assert(offsetof(FGpuSceneInstanceMeta, MaterialId) ==  4);
    static_assert(offsetof(FGpuSceneInstanceMeta, CellId)     ==  8);
    static_assert(offsetof(FGpuSceneInstanceMeta, Flags)      == 12);
    static_assert(offsetof(FGpuSceneInstanceMeta, Generation) == 16);

    inline bool operator==(const FGpuSceneInstanceMeta& A, const FGpuSceneInstanceMeta& B) {
        return A.MeshId == B.MeshId && A.MaterialId == B.MaterialId && A.CellId == B.CellId &&
               A.Flags == B.Flags && A.Generation == B.Generation;
    }
    inline bool operator!=(const FGpuSceneInstanceMeta& A, const FGpuSceneInstanceMeta& B) {
        return !(A == B);
    }

    // === O que de uma linha entra no LAYOUT de regioes (Fase 5) ================================
    //
    // A capacidade de cada lote conta as instancias vivas que o indireto emitiria (ver
    // `GpuCountRegionCapacities`, GpuClassify.h), e ela depende de poucos campos — SO deles. E isto
    // que deixa o layout ser refeito apenas quando a capacidade pode ter mudado: transform,
    // assentamento do previous e visibilidade reescrevem a linha de meta e NAO mudam a capacidade (a
    // instancia escondida continua reservando espaco, de proposito).
    //
    // ⚠️ Espelho dos campos que a porta do classify le. Um termo novo la exige um termo novo aqui, e o
    // teste que enumera os campos das linhas contra a capacidade e o que pega o esquecimento.

    // Da INSTANCIA: ocupacao (generation zero ou nao; o valor em si nao conta), associacao (malha e
    // material) e `RaytracingOnly`, que tira a linha da contagem. `CellId` e os outros bits ficam de
    // fora.
    inline bool GpuInstanceMetaAltersLayout(const FGpuSceneInstanceMeta& _Old,
                                            const FGpuSceneInstanceMeta& _New) {
        return (_Old.Generation != 0u) != (_New.Generation != 0u) ||
               _Old.MeshId != _New.MeshId || _Old.MaterialId != _New.MaterialId ||
               ((_Old.Flags ^ _New.Flags) & GpuSceneInstanceRaytracingOnly) != 0u;
    }

    // Do MATERIAL: so a classe de pipeline. Cor, textura e o resto sao edicao frequente e nao mudam
    // quem o indireto emite. Os bits de `Flags` que DERIVAM a classe chegam pela propria classe: a
    // linha e montada por `FromParts`, que carimba as duas coisas juntas.
    inline bool GpuMaterialAltersLayout(const FGpuMaterialRecord& _Old,
                                        const FGpuMaterialRecord& _New) {
        return _Old.PipelineClass != _New.PipelineClass;
    }

    struct FGpuSceneInstanceBounds {
        Vec4 Center{}; // xyz world-space; w reservado
        Vec4 Extent{}; // xyz meia-extensao; w reservado
    };
    static_assert(sizeof(FGpuSceneInstanceBounds) == 32);
    static_assert(offsetof(FGpuSceneInstanceBounds, Center) ==  0);
    static_assert(offsetof(FGpuSceneInstanceBounds, Extent) == 16);

    inline bool operator==(const FGpuSceneInstanceBounds& A,
                           const FGpuSceneInstanceBounds& B) {
        return A.Center.X == B.Center.X && A.Center.Y == B.Center.Y &&
               A.Center.Z == B.Center.Z && A.Center.W == B.Center.W &&
               A.Extent.X == B.Extent.X && A.Extent.Y == B.Extent.Y &&
               A.Extent.Z == B.Extent.Z && A.Extent.W == B.Extent.W;
    }
    inline bool operator!=(const FGpuSceneInstanceBounds& A,
                           const FGpuSceneInstanceBounds& B) { return !(A == B); }

    struct FGpuSceneInstanceTransform {
        Mat44 Value{};
    };
    static_assert(sizeof(FGpuSceneInstanceTransform) == 64);
    static_assert(offsetof(FGpuSceneInstanceTransform, Value) == 0);

    inline bool operator==(const FGpuSceneInstanceTransform& A,
                           const FGpuSceneInstanceTransform& B) {
        for (u32 Row = 0; Row < 4; ++Row)
            for (u32 Col = 0; Col < 4; ++Col)
                if (A.Value.M[Row][Col] != B.Value.M[Row][Col]) return false;
        return true;
    }
    inline bool operator!=(const FGpuSceneInstanceTransform& A,
                           const FGpuSceneInstanceTransform& B) { return !(A == B); }

    // === Snapshot por instancia =================================================================
    //
    // Lido pelo InstanceID da TLAS. Espelhado em HLSL como `InstanceGeo`.
    //
    // Depois da Fase 2 ele so carrega ENDERECO. Tinha 100 B e treze campos de material COPIADOS
    // por instancia — a duplicacao que obrigava toda edicao de material a reescrever a cena
    // inteira, num upload heap sem versao por frame, com as duas filas drenadas.
    //
    // Continua morando em upload heap e continua sendo montado pela FRaytracingScene: quem
    // preenche tem de ser o dono da correspondencia com a TLAS, e essa correspondencia (a ordem,
    // com os buracos preservados) e o contrato da Fase 1 que nao se mexe aqui.
    struct FGpuInstanceGeo {
        // DESNORMALIZADO de proposito, e e a unica excecao: o payload por triangulo e o caminho
        // MAIS QUENTE do ray tracing — o AlphaTestPass roda por CANDIDATO de travessia, nao por
        // hit comitado, entao em folhagem ele e chamado varias vezes por raio. Passar por
        // instancia -> mesh -> payload acrescentaria uma leitura dependente exatamente ali. A
        // decisao D3 do plano autoriza: tabela canonica, snapshot especializado.
        u32 TriangleSrv = 0;
        // Tabela de meshes. VB/IB e bounds vivem la; quem os le e caminho frio (MeshLightExtract,
        // uma vez por cena; o culling da Fase 6).
        u32 MeshSrv     = 0xFFFFFFFFu;
        u32 MeshId      = 0;
        u32 MaterialSrv = 0xFFFFFFFFu;
        u32 MaterialId  = 0; // FGpuMaterialTable::kDefaultRow
    };
    static_assert(sizeof(FGpuInstanceGeo) == 20, "FGpuInstanceGeo deve casar com o HLSL (20 B)");
    static_assert(offsetof(FGpuInstanceGeo, TriangleSrv) ==  0);
    static_assert(offsetof(FGpuInstanceGeo, MeshSrv)     ==  4);
    static_assert(offsetof(FGpuInstanceGeo, MeshId)      ==  8);
    static_assert(offsetof(FGpuInstanceGeo, MaterialSrv) == 12);
    static_assert(offsetof(FGpuInstanceGeo, MaterialId)  == 16);

    // === Diferenca de linhas ====================================================================
    //
    // Copia as linhas novas sobre o shadow e devolve os indices das que MUDARAM.
    //
    // Template livre, inline e CPU puro de proposito: e o coracao do gate "uma edicao toca uma
    // linha", e um teste dele nao pode depender de device, fila nem heap de descritores — nem de
    // linkar o .cpp desta tabela, que arrasta a fabrica de recursos inteira.
    //
    // Shadow menor que Fresh cresce; Shadow maior e truncado. Linha nova conta como mudada: a GPU
    // nunca a recebeu.
    //
    // ⚠️ O shadow e o que a GPU TEM. As tabelas NAO chamam isto: elas acham o que mudou com
    // `FindChangedRows`, gravam a copia, e so entao avancam o shadow com `CommitChangedRows`. Avancar
    // antes e publicar uma imagem que a GPU talvez nunca receba — a copia pode ser recusada.

    // O que MUDARIA, sem tocar no shadow.
    template <typename TRow>
    u32 FindChangedRows(std::span<const TRow> Fresh, std::span<const TRow> Shadow,
                        std::vector<u32>& OutChanged) {
        OutChanged.clear();
        for (u32 I = 0; I < static_cast<u32>(Fresh.size()); ++I)
            if (I >= Shadow.size() || Shadow[I] != Fresh[I]) OutChanged.push_back(I);
        return static_cast<u32>(OutChanged.size());
    }

    // Avanca o shadow: dimensiona pelo `Fresh` (cresce ou trunca) e copia as linhas mudadas.
    template <typename TRow>
    void CommitChangedRows(std::span<const TRow> Fresh, std::span<const u32> Changed,
                           std::vector<TRow>& Shadow) {
        if (Shadow.size() != Fresh.size()) Shadow.resize(Fresh.size());
        for (u32 I : Changed)
            if (I < Fresh.size()) Shadow[I] = Fresh[I];
    }

    template <typename TRow>
    u32 DiffTableRows(std::span<const TRow> Fresh, std::vector<TRow>& Shadow,
                      std::vector<u32>& OutChanged) {
        const u32 Mudadas = FindChangedRows<TRow>(Fresh, Shadow, OutChanged);
        CommitChangedRows<TRow>(Fresh, OutChanged, Shadow);
        return Mudadas;
    }

    // Mesma diferenca, restrita aos slots que o PRODUTOR tocou.
    //
    // Existe porque a versao acima varre `Fresh.size()`, e `Fresh` e dimensionada pela CAPACIDADE
    // da tabela, nao pelas linhas vivas: arrastar um objeto no gizmo com a sintetica de 1M carregada
    // fazia quatro memcmp de tabela inteira (~191 MB) por frame para achar UMA linha. O gate da
    // Fase 4 — "1M instancias nao exigem reconstrucao/upload integral por frame" — e exatamente
    // este numero.
    //
    // `Touched` tem de vir ORDENADO e sem repeticao: o `OutChanged` resultante herda a ordem, e o
    // RecordUpload depende dela para coalescer linhas contiguas numa copia so. Os slots vem de uma
    // free list, entao a ordem NAO e a de percurso da cena — quem monta ordena.
    //
    // O que NAO muda em relacao ao diff completo: um slot ausente de `Touched` tambem nao foi
    // escrito pelo produtor, entao seu par (linha, shadow) continua igual por construcao. O shadow
    // cresce ate `Fresh.size()` na primeira chamada para que os dois indices continuem falando do
    // mesmo slot.
    // `OutVisited` e ACUMULADO, uma unidade por comparacao efetivamente feita, e nao derivado de
    // `Touched.size()`. A diferenca e o ponto: um contador calculado a partir do conjunto de
    // entrada continuaria dizendo "1" se alguem trocasse este laco por um que varre a tabela
    // inteira — mediria a intencao, nao o trabalho. O gate da fase se decide por este numero,
    // entao ele conta de dentro do laco.
    //
    // A mesma separacao das duas fases: `FindChangedRowsAt` nao toca no shadow, e as tabelas o avancam
    // com `CommitChangedRows` depois da copia gravada.
    template <typename TRow>
    u32 FindChangedRowsAt(std::span<const TRow> Fresh, std::span<const u32> Touched,
                          std::span<const TRow> Shadow, std::vector<u32>& OutChanged,
                          u32& OutVisited) {
        OutChanged.clear();
        for (u32 Slot : Touched) {
            if (Slot >= Fresh.size()) continue;
            ++OutVisited;
            // Alem do shadow e a linha default: e o que o shadow teria ao crescer.
            const TRow Antes = Slot < Shadow.size() ? Shadow[Slot] : TRow{};
            if (Antes != Fresh[Slot]) OutChanged.push_back(Slot);
        }
        return static_cast<u32>(OutChanged.size());
    }

    template <typename TRow>
    u32 DiffTableRowsAt(std::span<const TRow> Fresh, std::span<const u32> Touched,
                        std::vector<TRow>& Shadow, std::vector<u32>& OutChanged,
                        u32& OutVisited) {
        const u32 Mudadas = FindChangedRowsAt<TRow>(Fresh, Touched, Shadow, OutChanged, OutVisited);
        CommitChangedRows<TRow>(Fresh, OutChanged, Shadow);
        return Mudadas;
    }

    inline u32 DiffMaterialRows(std::span<const FGpuMaterialRecord> Fresh,
                                std::vector<FGpuMaterialRecord>& Shadow,
                                std::vector<u32>& OutChanged) {
        return DiffTableRows<FGpuMaterialRecord>(Fresh, Shadow, OutChanged);
    }

    // === Footprint de uma tabela ================================================================
    //
    // Duas grandezas, porque nao saem do mesmo orcamento: o buffer DEFAULT ocupa VRAM, e os slices
    // de staging ocupam memoria do HOST. Os slices sao PERSISTENTES — criados junto com a tabela e
    // mapeados o tempo todo, nao alocados por upload —, e foi exatamente por esquece-los que o
    // primeiro `residentBytes` publicado valia um TERCO do que a tabela custava.
    //
    // ⚠️ Sao bytes de CONTEUDO (capacidade x stride), e nao custo de alocacao: o D3D12 arredonda
    // committed resource para a granularidade de heap, e com tabelas de poucos KB o arredondamento
    // domina. O custo real, com alinhamento, ja e medido pelo VramTracker, que registra o tamanho
    // que o driver cobra e da rotulo proprio as tres tabelas no breakdown por recurso. O que
    // ESTES numeros respondem e outra pergunta: como a tabela cresce com a cena.
    //
    // Funcao pura e livre para poder ser testada sem device: o `Create` que aloca de verdade exige
    // ID3D12Device, e uma conta que so existe la dentro nao tem como ser conferida.
    struct FGpuTableFootprint {
        u64 Resident = 0; // VRAM
        u64 Staging  = 0; // host
        u64 Total() const { return Resident + Staging; }
    };

    // === Armazenamento comum ====================================================================
    //
    // Buffer em DEFAULT heap + staging por frame em voo + o descritor. Sem tipo de linha: as duas
    // tabelas fazem a MESMA coisa com bytes, e duplicar barreira, coalescencia de intervalos e
    // ciclo de vida do descritor em cada uma seria duas chances de divergir.
    class FGpuTableStorage {
    public:
        // Slices de staging por frame em voo. Igual a FCommandQueue::kFramesInFlight (assert no
        // .cpp): a CPU escreve o slice deste frame enquanto a GPU ainda pode estar copiando o do
        // frame anterior.
        static constexpr u32 kFrameSlots = 2;
        // As tabelas sao compartilhadas por RT/compute e pelo raster. Depois de cada upload o
        // estado precisa autorizar os dois tipos de leitor; NON_PIXEL sozinho compila e costuma
        // desenhar, mas viola a validacao de estado quando G-buffer/shadow PS acessam a linha.
        static constexpr D3D12_RESOURCE_STATES kShaderReadState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // (Re)cria o buffer quando a capacidade cresce; o SLOT do descritor e preservado sempre.
        // Isso importa: o indice viaja dentro do InstanceGeo e e lido via ResourceDescriptorHeap,
        // entao trocar o recurso por baixo com o indice estavel e o que faz os leitores
        // acompanharem sem republicar tabela nenhuma.
        //
        // Atomico: quando lanca (recurso ou descritor recusado), o armazenamento fica exatamente como
        // estava — vazio, se era novo; com buffer, staging, capacidade e slot antigos, se crescia.
        //
        // ⚠️ CONTRATO: substitui um recurso que as duas filas podem estar lendo. O chamador drena
        // antes — e ja drena hoje, porque isto so acontece em carga/troca de cena.
        void Create(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 RowBytes, u32 Capacity,
                    const char* Label);
        // Nao aloca nem lanca: e o que os rollbacks de falta de memoria chamam.
        void Release(FTextureSRVHeap& SRVHeap) noexcept;

        // A copia destas linhas pode ser gravada? Lista, recurso e staging existem, e toda linha cabe
        // no buffer. Nada a copiar e aceito. Sem efeito nenhum: e a porta que as tabelas consultam
        // ANTES de gravar e de avancar o shadow, e a mesma que o `RecordUpload` aplica.
        bool AcceptsUpload(ID3D12GraphicsCommandList* CommandList, const void* Rows,
                           std::span<const u32> Changed) const noexcept;

        // Copia as linhas indicadas. Rows aponta para um array contiguo de pelo menos
        // max(Changed)+1 linhas. Devolve quantas regioes de copia foram emitidas (linhas
        // contiguas viram uma so); zero quando nao ha o que copiar ou quando a porta recusa, e a
        // recusa vem antes de qualquer escrita. Nao aloca: memcpy no staging e comandos na lista.
        u32 RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot, const void* Rows,
                         std::span<const u32> Changed) noexcept;

        // O recurso DEFAULT. Para leitura de conferencia; os leitores de frame usam o SRV.
        ID3D12Resource* Resource() const { return Buffer_.Get(); }

        bool IsBuilt()  const { return Buffer_ != nullptr; }
        u32  Srv()      const { return SrvSlot_; }
        u32  Capacity() const { return Capacity_; }
        u32  RowBytes() const { return RowBytes_; }

        FGpuTableFootprint Footprint() const { return FootprintFor(Capacity_, RowBytes_); }
        u64 ResidentBytes()   const { return Footprint().Resident; }
        u64 StagingBytes()    const { return Footprint().Staging; }
        // O numero que o gate da fase pede por tabela. Os dois separados continuam expostos
        // porque VRAM e memoria do host nao saem do mesmo orcamento.
        u64 PersistentBytes() const { return Footprint().Total(); }

        // A conta, isolada do recurso. Ver FGpuTableFootprint.
        static constexpr FGpuTableFootprint FootprintFor(u32 Capacity, u32 RowBytes) {
            const u64 Resident = static_cast<u64>(Capacity) * RowBytes;
            return { Resident, Resident * kFrameSlots };
        }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> Buffer_;
        GpuResources::FUploadBuffer            Staging_;
        u32                                    SrvSlot_  = 0xFFFFFFFFu;
        u32                                    Capacity_ = 0;
        u32                                    RowBytes_ = 0;
    };

    // === Tabela de materiais ====================================================================
    class FGpuMaterialTable {
    public:
        // Linha 0 e o material DEFAULT (renderavel sem material). Reservada, e nao "o primeiro
        // material": um MaterialId que caia em 0 por engano tem de produzir o cinza 0.7 do
        // snapshot antigo, e nao a aparencia de um material real da cena.
        static constexpr u32 kDefaultRow = 0;
        static constexpr u32 kRowBytes   = static_cast<u32>(sizeof(FGpuMaterialRecord));

        // Fail-closed: lancou, a tabela sai LIBERADA — sem mapa novo sobre armazenamento antigo e sem
        // descritor preso —, e `IsBuilt` so volta a valer quando um Setup chega ao fim.
        void SetupForMaterials(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               std::span<FMaterial* const> Materials);
        // Nao aloca nem lanca, como o do armazenamento: o rollback do Setup passa por aqui.
        void Release(FTextureSRVHeap& SRVHeap) noexcept;

        // "Algum material mudou." Nao diz qual, porque quem chama tambem nao sabe (ver a nota 2 do
        // topo). O conjunto real sai da comparacao feita no PrepareUpload. Invalida uma publicacao
        // ja preparada: ela descreve as linhas de antes.
        void MarkDirty() {
            Dirty_    = true;
            Prepared_ = false;
        }

        // A publicacao das linhas que mudaram, na command list do FRAME, em DUAS fases. Sem dreno,
        // sem Map do recurso final, sem barreira global.
        //
        // Tem de ser gravada ANTES do primeiro leitor do frame — o que inclui a fila de COMPUTE,
        // porque o update do DDGI le a tabela pela mesma indirecao. Isso e satisfeito gravando no
        // topo da command list da direta: o trabalho async e submetido com SubmitAfter sobre um
        // segmento posterior desta mesma lista.
        //
        // FASE 1, antes do BeginFrame: tudo o que aloca ou pode lancar — as linhas novas, o diff e a
        // reserva do shadow —, sem gravar nem avancar nada. Devolve se a copia sera aceita.
        // Fail-closed: tabela incompleta recusa. Nada pendente e aceito. Transacional: recusada, nada
        // anda, e o frame seguinte tenta de novo.
        bool PrepareUpload(ID3D12GraphicsCommandList* CommandList);
        // FASE 2, depois do BeginFrame: grava a copia preparada e avanca shadow e versao. NAO aloca
        // nem lanca. Sem uma preparacao valida para ESTA lista — nenhuma, ou invalidada por mutacao
        // da tabela —, nao grava nada e devolve false.
        bool RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot) noexcept;
        bool IsPreparedFor(ID3D12GraphicsCommandList* CommandList) const noexcept {
            return Prepared_ && PreparedList_ == CommandList;
        }

        u32  Srv()      const { return Storage_.Srv(); }
        u32  RowCount() const { return static_cast<u32>(RowMaterials_.size()); }
        // Setup COMPLETO e nao liberado. Nao e "o buffer existe": um Setup que lancou depois de criar
        // o buffer nao conta.
        bool IsBuilt()  const { return Built_; }

        // Linha de um material. Devolve kDefaultRow para nulo ou desconhecido — desconhecido
        // acontece de verdade: o proxy do terreno e materiais criados fora do import nao estao na
        // lista de materiais da cena.
        u32 RowOf(const FMaterial* Material) const;

        u64 PersistentBytes()    const { return Storage_.PersistentBytes(); }
        u64 ResidentBytes()      const { return Storage_.ResidentBytes(); }
        u64 StagingBytes()       const { return Storage_.StagingBytes(); }
        u32 LastDirtyRows()      const { return LastDirtyRows_; }
        u32 LastCopyRegions()    const { return LastCopyRegions_; }
        u64 TotalRowsUploaded()  const { return TotalRowsUploaded_; }

        // As linhas PUBLICADAS: o shadow, que so avanca depois de a copia ser GRAVADA na command list
        // do frame. Lidas depois do RecordUpload, sao as que a GPU tem quando essa lista executa — e
        // portanto as que os leitores gravados depois dele, na mesma lista, enxergam. Uma copia
        // recusada nao as avanca.
        std::span<const FGpuMaterialRecord> PublishedRows() const { return Shadow_; }
        // O recurso da tabela, para leitura de conferencia. Os leitores de frame usam o SRV.
        ID3D12Resource* Resource() const { return Storage_.Resource(); }

        // Sobe quando o que o layout de regioes conta mudou nas linhas publicadas: a classe de alguma
        // linha ou o numero de linhas. Uma edicao de cor nao a move.
        //
        // Monotonica pela vida da TABELA, atravessando Setup e Release: um contador que voltasse a
        // zero coincidiria com uma versao que o layout ja viu.
        u64 LayoutInputsVersion() const { return LayoutInputsVersion_; }

    private:
        FGpuTableStorage Storage_;
        // Ponteiro por linha. Guardado, e nao consultado no editor a cada upload, porque a ordem
        // das linhas e o contrato do MaterialId gravado no InstanceGeo.
        std::vector<FMaterial*>                  RowMaterials_;
        // O inverso, para o RowOf nao ser linear: ele e chamado uma vez por RENDERAVEL.
        std::unordered_map<const FMaterial*, u32> RowByMaterial_;
        // O que a GPU tem AGORA. A comparacao contra isto e o que transforma "algo mudou" em
        // "estas linhas mudaram".
        std::vector<FGpuMaterialRecord>          Shadow_;
        std::vector<FGpuMaterialRecord>          Fresh_;   // buffer de trabalho
        std::vector<u32>                         Changed_; // idem

        bool Built_             = false;
        bool Dirty_             = false;
        // A publicacao preparada: valida so para a lista em que foi preparada, e ate a proxima
        // mutacao da tabela.
        bool                       Prepared_             = false;
        bool                       PreparedAltersLayout_ = false;
        ID3D12GraphicsCommandList* PreparedList_         = nullptr;
        u32  LastDirtyRows_     = 0;
        u32  LastCopyRegions_   = 0;
        u64  TotalRowsUploaded_ = 0;
        u64  LayoutInputsVersion_ = 0;
    };

    // === Tabela de meshes =======================================================================
    //
    // Dona dos descritores bindless de geometria (tres por malha unica) e a UNICA enumeradora das
    // malhas unicas da cena. As duas coisas moravam no FRaytracingScene; a segunda ja tinha
    // causado um bug quando o FDDGI reconstruia a mesma lista com um criterio "identico, palavra
    // por palavra".
    class FGpuMeshTable {
    public:
        // Linha 0 e a malha AUSENTE, pelo mesmo motivo da linha 0 de material: um MeshId que caia
        // em 0 por engano tem de descrever geometria vazia, e nao a primeira malha da cena.
        static constexpr u32 kMissingRow = 0;
        static constexpr u32 kRowBytes   = static_cast<u32>(sizeof(FGpuMeshRecord));
        // Descritores por malha unica: [VB][IB][RTTri].
        static constexpr u32 kDescriptorsPerMesh = 3;

        // Enumera as malhas unicas da cena, aloca e escreve os descritores de geometria e monta as
        // linhas. Mesmo contrato de dreno do FGpuTableStorage::Create, e fail-closed como o do
        // material: lancou, sai liberada, com o range de descritores de geometria devolvido.
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, const FScene& Scene);
        void Release(FTextureSRVHeap& SRVHeap) noexcept;

        // Mesma publicacao em duas fases do FGpuMaterialTable: preparar antes do BeginFrame (aloca,
        // pode lancar, fail-closed), gravar depois (sem alocar nem lancar, so com preparacao valida).
        bool PrepareUpload(ID3D12GraphicsCommandList* CommandList);
        bool RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot) noexcept;
        bool IsPreparedFor(ID3D12GraphicsCommandList* CommandList) const noexcept {
            return Prepared_ && PreparedList_ == CommandList;
        }

        u32  Srv()      const { return Storage_.Srv(); }
        u32  RowCount() const { return static_cast<u32>(RowMeshes_.size()); }
        // Setup COMPLETO e nao liberado, como no FGpuMaterialTable.
        bool IsBuilt()  const { return Built_; }

        // Malhas unicas, na ordem em que aparecem na cena. NAO inclui a linha 0. E daqui que o
        // FRaytracingScene tira a lista para dimensionar os BLAS.
        std::span<const FGpuMesh* const> Meshes() const { return UniqueMeshes_; }

        u32 RowOf(const FGpuMesh* Mesh) const;
        // Primeiro dos tres descritores de geometria da malha (VB; IB = +1, RTTri = +2).
        // Devolve 0 para malha desconhecida — o MESMO valor que o mapa anterior deixava, e
        // inalcancavel na pratica: renderavel sem malha valida nao entra na TLAS, entao nenhum hit
        // enderaca a linha dele. Trocar por 0xFFFFFFFF pareceria mais seguro e seria pior: um
        // ResourceDescriptorHeap[0xFFFFFFFF] trava a GPU em vez de ler lixo.
        u32 GeometrySlotOf(const FGpuMesh* Mesh) const;

        // Pools de geometria da cena, indexados pelo `GeometryPoolId` da linha (Fase 5).
        //
        // O id na linha e inutil sem isto: um `ExecuteIndirect` amarra UM par VB/IB, e e a CPU que
        // precisa amarrar o do pool antes de cada chamada. Publicar so o id deixaria um numero sem
        // referente.
        //
        // Ponteiros CRUS, e vivos enquanto o setup vale: cada FGpuMesh segura os ComPtrs dos proprios
        // recursos, e as malhas do `UniqueMeshes_` vivem na cena. Um Release/SetupForScene refaz a
        // lista.
        std::span<const FGpuGeometryPool> GeometryPools() const { return GeometryPools_; }

        u64 PersistentBytes()   const { return Storage_.PersistentBytes(); }
        u64 ResidentBytes()     const { return Storage_.ResidentBytes(); }
        u64 StagingBytes()      const { return Storage_.StagingBytes(); }
        u32 LastDirtyRows()     const { return LastDirtyRows_; }
        u32 DescriptorCount()   const { return DescriptorCount_; }
        // Malhas que ficaram fora do caminho indireto por offset inalcancavel. Contador, e nao
        // booleano: o numero diz quantas, e zero e o estado esperado em toda cena de hoje.
        u32 UnrepresentableOffsets() const { return UnrepresentableOffsets_; }
        // Malhas fora do indireto por IndexCount que nao descreve triangulos inteiros. Separado do
        // de cima porque as causas sao diferentes: offset e o pool tendo crescido, contagem e o
        // cozido. Zero e o estado esperado nas cenas de hoje.
        u32 UnusableIndexCounts() const { return UnusableIndexCounts_; }

        // As linhas PUBLICADAS, como no FGpuMaterialTable.
        std::span<const FGpuMeshRecord> PublishedRows() const { return Shadow_; }
        ID3D12Resource* Resource() const { return Storage_.Resource(); }

        // Sobe a CADA republicacao que muda alguma linha, e no Setup. Mais grossa que a do material de
        // proposito: linha de malha so muda por SetupForScene, que limpa o shadow, entao pool e mascara
        // de malha inutilizavel nunca mudam sozinhos. Uma comparacao por campo aqui seria defesa sem
        // caminho que a alcance. O Release nao a move: ele esvazia os pools, e o layout ja compara o
        // numero de pools. Monotonica pela vida da tabela.
        u64 LayoutInputsVersion() const { return LayoutInputsVersion_; }

    private:
        // O corpo do Setup, tudo o que pode lancar. Separado para o SetupForScene envolve-lo inteiro.
        void BuildForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, const FScene& Scene);

        FGpuTableStorage Storage_;
        std::vector<const FGpuMesh*>              UniqueMeshes_;
        std::vector<FGpuGeometryPool>             GeometryPools_;
        u32                                       UnrepresentableOffsets_ = 0;
        u32                                       UnusableIndexCounts_    = 0;
        std::vector<const FGpuMesh*>              RowMeshes_;   // [0] = nullptr (linha ausente)
        std::unordered_map<const FGpuMesh*, u32>  RowByMesh_;
        std::vector<FGpuMeshRecord>               Shadow_;
        std::vector<FGpuMeshRecord>               Fresh_;
        std::vector<u32>                          Changed_;

        u32 DescriptorBase_  = 0xFFFFFFFFu;
        u32 DescriptorCount_ = 0;
        bool Built_          = false;
        bool Dirty_          = false;
        bool                       Prepared_          = false;
        bool                       PreparedPublishes_ = false;
        ID3D12GraphicsCommandList* PreparedList_      = nullptr;
        u32  LastDirtyRows_  = 0;
        u64  LayoutInputsVersion_ = 0;
    };

    // === Tabela de instancias ==================================================================
    //
    // Quatro buffers DEFAULT independentes. Separar meta/bounds/matrizes evita que o culling leia
    // 128 B de transform para testar uma caixa. O estado CPU abaixo e a fonte unica do historico:
    // raster e temporal derivam suas visoes especializadas daqui, em vez de manter dois mapas de
    // previous model que podiam divergir. O historico e o PUBLICADO: o previous de cada ocupante e
    // o current da ultima publicacao aceita, e o raster CPU le o par pela mesma imagem.
    class FGpuSceneInstanceTable {
    public:
        static constexpr u32 kMetaBytes      = sizeof(FGpuSceneInstanceMeta);
        static constexpr u32 kBoundsBytes    = sizeof(FGpuSceneInstanceBounds);
        static constexpr u32 kTransformBytes = sizeof(FGpuSceneInstanceTransform);
        static constexpr u32 kResidentBytesPerSlot =
            kMetaBytes + kBoundsBytes + 2u * kTransformBytes; // 180 B

        // Fail-closed como as outras: lancou em qualquer das quatro colunas, ou nos vetores de CPU, a
        // tabela sai liberada e `IsBuilt` so volta a valer no fim de um Setup.
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, const FScene& Scene);
        void Release(FTextureSRVHeap& SRVHeap) noexcept;

        // Atualiza a imagem CPU apenas quando estrutura/transform/static-casters mudaram ou quando
        // o previous transform precisa assentar em current no frame seguinte. No frame estavel nao
        // ha varredura nem upload.
        //
        // A cena entra por referencia NAO-CONST por um motivo so: este e o consumidor unico da
        // caixa de correio de sujos (FScene::DirtyRenderableHints), e le-la esvazia-a. Quando a
        // caixa esta completa, o trabalho e proporcional a quem mudou; quando nao esta — bump sem
        // dica, mudanca de estrutura, primeira publicacao —, cai na varredura completa. Nenhum
        // outro campo da cena e tocado.
        void PrepareFrame(FScene& Scene, const FGpuMeshTable& Meshes,
                          const FGpuMaterialTable& Materials);
        // Mesma publicacao em duas fases do FGpuMaterialTable. A preparacao faz o diff das quatro
        // colunas sobre os tocados do ultimo PrepareFrame, e o produtor a invalida: um ciclo novo muda
        // linhas e tocados.
        bool PrepareUpload(ID3D12GraphicsCommandList* CommandList);
        bool RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot) noexcept;
        bool IsPreparedFor(ID3D12GraphicsCommandList* CommandList) const noexcept {
            return Prepared_ && PreparedList_ == CommandList;
        }

        // Setup COMPLETO e nao liberado: as QUATRO colunas e os vetores de CPU. Conferir so a meta
        // aprovava uma tabela cujo Setup lancou depois de cria-la.
        bool IsBuilt() const { return Built_; }
        // A cena do ultimo PrepareFrame cabe na tabela. Falso quando ela cresceu sem o Setup que
        // acompanha o crescimento — ai o produtor nao publica, e a porta recusa o frame.
        bool FitsScene() const { return FitsScene_; }
        bool NeedsCapacity(u32 RequiredSlots) const {
            return !IsBuilt() || RequiredSlots > Capacity();
        }
        u32 Capacity() const { return MetaStorage_.Capacity(); }
        u32 LiveRows() const { return LiveRows_; }

        u32 MetaSrv()          const { return MetaStorage_.Srv(); }
        u32 BoundsSrv()        const { return BoundsStorage_.Srv(); }
        u32 TransformSrv()     const { return TransformStorage_.Srv(); }
        u32 PrevTransformSrv() const { return PrevTransformStorage_.Srv(); }

        // O par PUBLICADO — o mesmo que os leitores da GPU tem neste frame —, validando
        // slot+generation contra a meta publicada. Consumido pelo ObjectCB, que precisa desenhar a
        // mesma imagem que o caminho bindless e os temporais. Depois de um upload aceito coincide com
        // o que o PrepareFrame escreveu; depois de uma recusa, e o par antigo, e um ocupante que nunca
        // foi publicado nao responde.
        bool TransformsFor(FGpuSceneHandle Handle, Mat44& Current, Mat44& Previous) const;
        // A consulta CPU existe para diagnostico/teste do mesmo contrato de identidade, e le a mesma
        // imagem publicada. Os consumidores de frame leem o SRV; nenhum deles deve contornar a tabela
        // por este atalho.
        bool MetaFor(FGpuSceneHandle Handle, FGpuSceneInstanceMeta& Meta) const;

        u64 ResidentBytes() const;
        u64 StagingBytes() const;
        u32 LastDirtyRows() const { return LastDirtyRows_; }
        u64 LastDirtyBytes() const { return LastDirtyBytes_; }
        u32 LastCopyRegions() const { return LastCopyRegions_; }
        u64 TotalBytesUploaded() const { return TotalBytesUploaded_; }
        // Comparacoes que o diff FEZ neste frame, contadas de dentro do laco. Uma por (slot,
        // coluna): um slot visitado vale quatro, porque as quatro colunas sao comparadas.
        //
        // E o numero que separa "upload incremental" de "trabalho incremental": as linhas sujas ja
        // eram poucas antes da Fase 4, mas o custo de descobri-las era a capacidade da tabela. Num
        // frame sem edicao vale zero, porque o produtor nem chega ao diff.
        u32 LastRowsScanned() const { return LastRowsScanned_; }
        static constexpr u32 kColumns = 4; // meta, bounds, transform, prev transform

        // A coluna de meta PUBLICADA: o shadow, que so avanca depois de as quatro copias serem GRAVADAS
        // (ver FGpuMaterialTable::PublishedRows). E a que o layout de regioes e a divisao
        // indireto/fallback leem, porque e a que o classify desse frame le.
        //
        // ⚠️ Igual a GPU nos slots que a lista de candidatos alcanca. Numa troca para cena MENOR o
        // `FGpuTableStorage::Create` preserva o buffer e o shadow recomeca zerado, entao um slot que
        // ficou morto pode guardar na GPU a linha da cena anterior. Nenhum leitor da cadeia chega nele:
        // expand e classify so leem slots de candidatos, que sao vivos.
        std::span<const FGpuSceneInstanceMeta> PublishedMeta() const { return MetaShadow_; }
        // As outras tres colunas PUBLICADAS, com a mesma ressalva. Nenhum leitor de producao as
        // consome por aqui — o layout so precisa da meta —, mas sem elas uma leitura de conferencia
        // nao tem contra o que comparar bounds e o par temporal.
        std::span<const FGpuSceneInstanceBounds> PublishedBounds() const { return BoundsShadow_; }
        std::span<const FGpuSceneInstanceTransform> PublishedTransforms() const {
            return TransformShadow_;
        }
        std::span<const FGpuSceneInstanceTransform> PublishedPrevTransforms() const {
            return PrevTransformShadow_;
        }
        // Os recursos das quatro colunas, para leitura de conferencia. Os leitores de frame usam o SRV.
        ID3D12Resource* MetaResource() const { return MetaStorage_.Resource(); }
        ID3D12Resource* BoundsResource() const { return BoundsStorage_.Resource(); }
        ID3D12Resource* TransformResource() const { return TransformStorage_.Resource(); }
        ID3D12Resource* PrevTransformResource() const { return PrevTransformStorage_.Resource(); }

        // Sobe quando alguma linha PUBLICADA muda o que o layout conta (`GpuInstanceMetaAltersLayout`),
        // e em Setup e Release. Transform, assentamento e visibilidade nao a movem. Monotonica pela
        // vida da tabela.
        u64 LayoutInputsVersion() const { return LayoutInputsVersion_; }

    private:
        FGpuTableStorage MetaStorage_;
        FGpuTableStorage BoundsStorage_;
        FGpuTableStorage TransformStorage_;
        FGpuTableStorage PrevTransformStorage_;

        std::vector<FGpuSceneInstanceMeta>      MetaRows_;
        std::vector<FGpuSceneInstanceBounds>    BoundsRows_;
        std::vector<FGpuSceneInstanceTransform> TransformRows_;
        std::vector<FGpuSceneInstanceTransform> PrevTransformRows_;

        std::vector<FGpuSceneInstanceMeta>      MetaShadow_;
        std::vector<FGpuSceneInstanceBounds>    BoundsShadow_;
        std::vector<FGpuSceneInstanceTransform> TransformShadow_;
        std::vector<FGpuSceneInstanceTransform> PrevTransformShadow_;

        std::vector<u32> MetaChanged_;
        std::vector<u32> BoundsChanged_;
        std::vector<u32> TransformChanged_;
        std::vector<u32> PrevTransformChanged_;
        std::vector<u32> LiveSlots_;
        std::vector<u32> NextLiveSlots_;
        // Slots escritos ou limpos pelo PrepareFrame deste ciclo, ordenados e sem repeticao. E a
        // ponte entre produtor e diff: sem ela o diff teria de redescobrir por varredura o que o
        // produtor acabou de saber por construcao.
        std::vector<u32> TouchedSlots_;
        // Slots cujo previous transform ainda nao alcancou o current PUBLICADO — inclusive os de um
        // movimento cuja copia foi recusada, que continuam aqui ate a publicacao ser aceita. Sem esta
        // lista, o assentamento do frame seguinte precisaria revarrer a cena so para descobrir quem
        // ficou para tras — o que devolveria, pelo caminho do assentamento, o custo que a caixa de
        // correio tirou.
        std::vector<u32> SettleSlots_;
        std::vector<u32> NextSettleSlots_;
        std::vector<u32> SeenEpoch_;
        std::vector<u8>  DirtyMarks_;

        u64 PreparedStructureVersion_ = ~0ull;
        u64 PreparedTransformVersion_ = ~0ull;
        u64 PreparedStaticCastersVersion_ = ~0ull;
        u64 LayoutInputsVersion_       = 0;
        u32 PrepareEpoch_              = 0;
        u32 LiveRows_                  = 0;
        bool NeedsSettle_              = false;
        bool Dirty_                    = false;
        bool Built_                    = false;
        bool FitsScene_                = true;
        bool Prepared_                 = false;
        bool PreparedAltersLayout_     = false;
        ID3D12GraphicsCommandList* PreparedList_ = nullptr;

        u32 LastDirtyRows_       = 0;
        u64 LastDirtyBytes_      = 0;
        u32 LastCopyRegions_     = 0;
        u32 LastRowsScanned_     = 0;
        u64 TotalBytesUploaded_  = 0;
    };

    // === A publicacao do FRAME: as tres tabelas, ou nenhum frame (Fase 5) ========================
    //
    // Cada tabela e transacional sozinha, mas isso nao basta para o frame. Se uma recusar e o frame
    // seguir, os leitores misturam duas imagens: o `TransformsFor` devolve o transform publicado
    // antigo enquanto frustum, caixa, material e associacao saem do FRenderable novo — e um objeto
    // nunca publicado vai pelo ObjectCB sem linha valida no caminho bindless. Descartar o frame DEPOIS
    // de gravar tambem nao serve: as tabelas que aceitaram ja teriam avancado o shadow sobre uma
    // copia que a GPU nunca executa.
    //
    // Por isso a publicacao tem DUAS fases. `GpuScenePreparePublication` roda com a command list
    // ainda fechada e faz tudo o que aloca ou pode lancar nas tres tabelas — diffs, reservas, portas.
    // Recusada, o Renderer nao abre o frame, e nada foi gravado nem avancou. `GpuSceneRecordPublication`
    // roda depois do BeginFrame e so grava e avanca: sem alocacao e sem excecao, porque uma excecao ali
    // deixaria a lista aberta com parte dos shadows ja avancados.
    //
    // FAIL-CLOSED: uma tabela incompleta — Setup que lancou, ou liberada — recusa o frame antes de
    // qualquer pergunta sobre copia, e a de instancias tambem recusa quando a cena nao cabe nela.
    // Devolve o primeiro motivo encontrado.
    enum EGpuScenePublication : u32 {
        GpuScenePublicationOk                 = 0,
        GpuScenePublicationRefusedMaterials   = 1,
        GpuScenePublicationRefusedMeshes      = 2,
        GpuScenePublicationRefusedInstances   = 3,
        GpuScenePublicationMaterialsNotBuilt  = 4,
        GpuScenePublicationMeshesNotBuilt     = 5,
        GpuScenePublicationInstancesNotBuilt  = 6,
        GpuScenePublicationSceneExceedsTable  = 7,
    };

    // FASE 1: antes do BeginFrame, depois do PrepareFrame das instancias.
    inline u32 GpuScenePreparePublication(FGpuMaterialTable& Materials, FGpuMeshTable& Meshes,
                                          FGpuSceneInstanceTable& Instances,
                                          ID3D12GraphicsCommandList* CommandList) {
        if (!Materials.IsBuilt()) return GpuScenePublicationMaterialsNotBuilt;
        if (!Meshes.IsBuilt()) return GpuScenePublicationMeshesNotBuilt;
        if (!Instances.IsBuilt()) return GpuScenePublicationInstancesNotBuilt;
        if (!Instances.FitsScene()) return GpuScenePublicationSceneExceedsTable;
        if (!Materials.PrepareUpload(CommandList)) return GpuScenePublicationRefusedMaterials;
        if (!Meshes.PrepareUpload(CommandList)) return GpuScenePublicationRefusedMeshes;
        if (!Instances.PrepareUpload(CommandList)) return GpuScenePublicationRefusedInstances;
        return GpuScenePublicationOk;
    }

    // FASE 2: depois do BeginFrame, com a MESMA lista. Confere as tres preparacoes antes de gravar
    // qualquer uma — uma invalida descoberta no meio deixaria as anteriores publicadas — e so entao
    // grava as tres. Devolve false, sem gravar nada, se alguma nao vale para esta lista.
    inline bool GpuSceneRecordPublication(FGpuMaterialTable& Materials, FGpuMeshTable& Meshes,
                                          FGpuSceneInstanceTable& Instances,
                                          ID3D12GraphicsCommandList* CommandList,
                                          u32 FrameSlot) noexcept {
        if (!Materials.IsPreparedFor(CommandList) || !Meshes.IsPreparedFor(CommandList) ||
            !Instances.IsPreparedFor(CommandList))
            return false;
        Materials.RecordUpload(CommandList, FrameSlot);
        Meshes.RecordUpload(CommandList, FrameSlot);
        Instances.RecordUpload(CommandList, FrameSlot);
        return true;
    }

    // O motivo, para o log de quem aborta o frame.
    inline const char* GpuScenePublicationReason(u32 Publication) {
        switch (Publication) {
        case GpuScenePublicationOk:                return "publicada";
        case GpuScenePublicationRefusedMaterials:  return "copia da tabela de materiais recusada";
        case GpuScenePublicationRefusedMeshes:     return "copia da tabela de malhas recusada";
        case GpuScenePublicationRefusedInstances:  return "copia da tabela de instancias recusada";
        case GpuScenePublicationMaterialsNotBuilt: return "tabela de materiais incompleta";
        case GpuScenePublicationMeshesNotBuilt:    return "tabela de malhas incompleta";
        case GpuScenePublicationInstancesNotBuilt: return "tabela de instancias incompleta";
        case GpuScenePublicationSceneExceedsTable: return "a cena nao cabe na tabela de instancias";
        default:                                   return "motivo desconhecido";
        }
    }

    // (Re)monta as tres tabelas da cena como UMA transacao: as tres ficam prontas para a MESMA cena,
    // ou as tres saem liberadas e a excecao segue. Sem isso, o material podia completar e a malha
    // falhar, e a porta de cada tabela, sozinha, nao ve que as duas descrevem cenas diferentes.
    // Mesmo contrato de dreno dos Setup.
    void GpuSceneSetupTables(FGpuMaterialTable& Materials, FGpuMeshTable& Meshes,
                             FGpuSceneInstanceTable& Instances, ID3D12Device* Device,
                             FTextureSRVHeap& SRVHeap, std::span<FMaterial* const> MaterialList,
                             const FScene& Scene);
    // Nao aloca nem lanca: e o rollback da transacao, e uma liberacao interrompida no meio deixaria
    // as tabelas seguintes intactas.
    void GpuSceneReleaseTables(FGpuMaterialTable& Materials, FGpuMeshTable& Meshes,
                               FGpuSceneInstanceTable& Instances, FTextureSRVHeap& SRVHeap) noexcept;
}
