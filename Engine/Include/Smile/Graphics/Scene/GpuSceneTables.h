#pragma once

// Tabelas CANONICAS residentes na GPU (Fase 2 do GPU-DRIVEN-PLAN).
//
// Duas hoje: materiais e meshes. A de instancias vem na Fase 4, quando houver culling na GPU para
// consumi-la; este header e a casa dela tambem.
//
// O que a de MATERIAIS resolve, em uma frase: editar um material passa a tocar UMA linha de 64 B
// em vez de reescrever o snapshot inteiro da cena (2.479 registros de 100 B na Emerald) com as
// duas filas drenadas.
//
// O que a de MESHES resolve e diferente, e vale ser honesto: ela NAO economiza byte nenhum no
// snapshot por instancia — VB/IB saem, MeshSrv/MeshId entram. O ganho e ter uma casa unica para
// bounds locais, contagens e os descritores bindless de geometria, que e o que o culling da Fase 6
// e a tabela de instancias da Fase 4 vao ler. E ela toma a posse dos descritores de geometria, que
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
#include "Smile/Math/Vec3.h"

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
        // do SMeshEntry no load). Aqui e a casa certa; a duplicacao no renderavel sai quando a
        // tabela de instancias existir, na Fase 4.
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

        // RESERVADO. Zero em toda malha, e assim fica ate a trilha de virtual geometry existir
        // (Fase 9+). Declarado agora porque o plano manda declara-lo, e NAO serializado no formato
        // cozido pelo mesmo motivo: nao ha consumidor. Ver a nota de decisao no GPU-DRIVEN-PLAN.
        u32 Flags             = 0;
        u32 VirtualGeometryId = 0;
    };
    static_assert(sizeof(FGpuMeshRecord) == 56, "FGpuMeshRecord deve casar com o HLSL (56 B)");
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

    inline bool operator==(const FGpuMeshRecord& A, const FGpuMeshRecord& B) {
        auto Same3 = [](const Vec3& L, const Vec3& R) {
            return L.X == R.X && L.Y == R.Y && L.Z == R.Z;
        };
        return Same3(A.LocalAABBMin, B.LocalAABBMin) && Same3(A.LocalAABBMax, B.LocalAABBMax) &&
               A.VertexSrv == B.VertexSrv && A.IndexSrv == B.IndexSrv &&
               A.TriangleSrv == B.TriangleSrv && A.VertexCount == B.VertexCount &&
               A.IndexCount == B.IndexCount && A.TriangleCount == B.TriangleCount &&
               A.Flags == B.Flags && A.VirtualGeometryId == B.VirtualGeometryId;
    }
    inline bool operator!=(const FGpuMeshRecord& A, const FGpuMeshRecord& B) { return !(A == B); }

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
    template <typename TRow>
    u32 DiffTableRows(std::span<const TRow> Fresh, std::vector<TRow>& Shadow,
                      std::vector<u32>& OutChanged) {
        OutChanged.clear();
        const u32 Count = static_cast<u32>(Fresh.size());
        const u32 Known = static_cast<u32>(Shadow.size() < Count ? Shadow.size() : Count);
        for (u32 I = 0; I < Known; ++I) {
            if (Shadow[I] != Fresh[I]) {
                Shadow[I] = Fresh[I];
                OutChanged.push_back(I);
            }
        }
        if (Shadow.size() != Count) Shadow.resize(Count);
        for (u32 I = Known; I < Count; ++I) {
            Shadow[I] = Fresh[I];
            OutChanged.push_back(I);
        }
        return static_cast<u32>(OutChanged.size());
    }

    inline u32 DiffMaterialRows(std::span<const FGpuMaterialRecord> Fresh,
                                std::vector<FGpuMaterialRecord>& Shadow,
                                std::vector<u32>& OutChanged) {
        return DiffTableRows<FGpuMaterialRecord>(Fresh, Shadow, OutChanged);
    }

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

        // (Re)cria o buffer quando a capacidade cresce; o SLOT do descritor e preservado sempre.
        // Isso importa: o indice viaja dentro do InstanceGeo e e lido via ResourceDescriptorHeap,
        // entao trocar o recurso por baixo com o indice estavel e o que faz os leitores
        // acompanharem sem republicar tabela nenhuma.
        //
        // ⚠️ CONTRATO: substitui um recurso que as duas filas podem estar lendo. O chamador drena
        // antes — e ja drena hoje, porque isto so acontece em carga/troca de cena.
        void Create(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 RowBytes, u32 Capacity,
                    const char* Label);
        void Release(FTextureSRVHeap& SRVHeap);

        // Copia as linhas indicadas. Rows aponta para um array contiguo de pelo menos
        // max(Changed)+1 linhas. Devolve quantas regioes de copia foram emitidas (linhas
        // contiguas viram uma so).
        u32 RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot, const void* Rows,
                         std::span<const u32> Changed);

        bool IsBuilt()  const { return Buffer_ != nullptr; }
        u32  Srv()      const { return SrvSlot_; }
        u32  Capacity() const { return Capacity_; }
        u32  RowBytes() const { return RowBytes_; }
        u64  PersistentBytes() const { return static_cast<u64>(Capacity_) * RowBytes_; }

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

        void SetupForMaterials(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               std::span<FMaterial* const> Materials);
        void Release(FTextureSRVHeap& SRVHeap);

        // "Algum material mudou." Nao diz qual, porque quem chama tambem nao sabe (ver a nota 2 do
        // topo). O conjunto real sai da comparacao feita no RecordUpload.
        void MarkDirty() { Dirty_ = true; }

        // Sobe as linhas que mudaram, na command list do FRAME. Sem dreno, sem Map do recurso
        // final, sem barreira global.
        //
        // Tem de ser gravado ANTES do primeiro leitor do frame — o que inclui a fila de COMPUTE,
        // porque o update do DDGI le a tabela pela mesma indirecao. Isso e satisfeito gravando no
        // topo da command list da direta: o trabalho async e submetido com SubmitAfter sobre um
        // segmento posterior desta mesma lista.
        void RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot);

        u32  Srv()      const { return Storage_.Srv(); }
        u32  RowCount() const { return static_cast<u32>(RowMaterials_.size()); }
        bool IsBuilt()  const { return Storage_.IsBuilt(); }

        // Linha de um material. Devolve kDefaultRow para nulo ou desconhecido — desconhecido
        // acontece de verdade: o proxy do terreno e materiais criados fora do import nao estao na
        // lista de materiais da cena.
        u32 RowOf(const FMaterial* Material) const;

        u64 PersistentBytes()    const { return Storage_.PersistentBytes(); }
        u32 LastDirtyRows()      const { return LastDirtyRows_; }
        u32 LastCopyRegions()    const { return LastCopyRegions_; }
        u64 TotalRowsUploaded()  const { return TotalRowsUploaded_; }

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

        bool Dirty_             = false;
        u32  LastDirtyRows_     = 0;
        u32  LastCopyRegions_   = 0;
        u64  TotalRowsUploaded_ = 0;
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
        // linhas. Mesmo contrato de dreno do FGpuTableStorage::Create.
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, const FScene& Scene);
        void Release(FTextureSRVHeap& SRVHeap);

        void RecordUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot);

        u32  Srv()      const { return Storage_.Srv(); }
        u32  RowCount() const { return static_cast<u32>(RowMeshes_.size()); }
        bool IsBuilt()  const { return Storage_.IsBuilt(); }

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

        u64 PersistentBytes()   const { return Storage_.PersistentBytes(); }
        u32 LastDirtyRows()     const { return LastDirtyRows_; }
        u32 DescriptorCount()   const { return DescriptorCount_; }

    private:
        FGpuTableStorage Storage_;
        std::vector<const FGpuMesh*>              UniqueMeshes_;
        std::vector<const FGpuMesh*>              RowMeshes_;   // [0] = nullptr (linha ausente)
        std::unordered_map<const FGpuMesh*, u32>  RowByMesh_;
        std::vector<FGpuMeshRecord>               Shadow_;
        std::vector<FGpuMeshRecord>               Fresh_;
        std::vector<u32>                          Changed_;

        u32 DescriptorBase_  = 0xFFFFFFFFu;
        u32 DescriptorCount_ = 0;
        bool Dirty_          = false;
        u32  LastDirtyRows_  = 0;
    };
}
