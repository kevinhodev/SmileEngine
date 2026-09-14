// Classificacao, regioes, scatter e finalizacao, executados em WARP (Fase 5 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir, em ordem de gravidade:
//
//  1. um comando cair na REGIAO de outro lote. Cada `ExecuteIndirect` amarra o par VB/IB do pool do
//     seu lote, e um comando fora da regiao certa desenha outra geometria sem erro de API nenhum;
//  2. um candidato receber DOIS resultados de classificacao, ou nenhum. A contabilidade e o que
//     permite auditar a fatia; ela NAO prova "desenhado exatamente uma vez" — ver a nota do
//     GpuClassify.h sobre o que a soma prova e o que ela nao prova;
//  3. a leitura de uma linha ANTES de validar o indice. Load fora do StructuredBuffer nao trava —
//     o D3D12 devolve zero —, e zero e a linha do material default e a da malha ausente. O
//     candidato passaria a desenhar algo PLAUSIVEL e errado;
//  4. a porta do indireto ser partida em dois testes. So o predicado completo pode decidir; a
//     decomposicao existe para ATRIBUIR o motivo, e um dia em que ela decidisse mandaria a malha
//     irrepresentavel para o command processor com offset poison;
//  5. os indices dos desfechos divergirem entre C++ e HLSL. Sao dois espelhos de macros e nada os
//     liga em tempo de compilacao; aqui os nove sao lidos de volta.
//
// As regioes que a GPU recebe saem SEMPRE das funcoes de producao, calculadas das TABELAS da cena. O
// fixture nao fornece offset nenhum: ele so os confere contra um oraculo escrito a mao. As unicas
// tabelas de regiao montadas a mao sao as dos cenarios de falha, e cada uma diz por que a producao
// nao a produziria.
//
// O shader e o de PRODUCAO, compilado pelo MESMO DXC dos shaders do jogo. WARP nao exige placa.
//
// A camada de debug PARTICIPA do resultado: ERROR e CORRUPTION reprovam, e sao drenados DEPOIS da
// fence — antes dela a validacao da execucao ainda nao existe.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Scene/GpuClassify.h"
#include "Smile/Graphics/Scene/GpuSceneCandidates.h"

namespace wrl = Microsoft::WRL;
using namespace Smile;

namespace {
    int Failures = 0;

    void Check(bool _Condition, std::string_view _Message) {
        if (!_Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << "\n";
        }
    }

    void CheckEq(u32 _Got, u32 _Want, std::string_view _Message) {
        if (_Got != _Want) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << " (obtido " << _Got << ", esperado " << _Want
                      << ")\n";
        }
    }


    // Versao u64 do CheckEq. Existe porque `GpuBatchSlotCount` e as capacidades de regiao sao u64 de
    // proposito: comparar depois de espremer em u32 seria testar exatamente o bug que eles evitam.
    void CheckEq64(u64 _Got, u64 _Want, std::string_view _Message) {
        if (_Got != _Want) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << " (obtido " << _Got << ", esperado " << _Want
                      << ")\n";
        }
    }
    void Hr(HRESULT _H, const char* _Onde) {
        if (FAILED(_H)) {
            char Buf[32];
            std::snprintf(Buf, sizeof(Buf), " falhou (0x%08X)", static_cast<unsigned>(_H));
            throw std::runtime_error(std::string(_Onde) + Buf);
        }
    }

    std::vector<u8> LerArquivo(const char* _Caminho) {
        std::ifstream F(_Caminho, std::ios::binary);
        if (!F) return {};
        return std::vector<u8>((std::istreambuf_iterator<char>(F)),
                               std::istreambuf_iterator<char>());
    }

    const char* NomeDesfecho(u32 _O) {
        switch (_O) {
            case GpuClassifyIndirect:          return "Indirect";
            case GpuClassifyNoSlot:            return "NoSlot";
            case GpuClassifyInvalidSlot:       return "InvalidSlot";
            case GpuClassifyInvalidMaterialId: return "InvalidMaterialId";
            case GpuClassifyInvalidMeshId:     return "InvalidMeshId";
            case GpuClassifyInvalidPoolId:     return "InvalidPoolId";
            case GpuClassifyNotEligible:       return "NotEligible";
            case GpuClassifyMeshUnusable:   return "Unrepresentable";
            case GpuClassifyOverflow:          return "Overflow";
            default:                           return "?";
        }
    }


    // Capacidade de lote da cena do teste, pela PORTA — nunca por
    // `static_cast<u32>(GpuBatchSlotCount(...))`. O contrato diz que a conversao para u32 so pode
    // acontecer dentro de `GpuClassifyBatchBufferSlots`, e um harness que a escreve por fora
    // mascara exatamente o overflow que motivou a porta. Se a porta recusasse, o teste para aqui em
    // vez de seguir com um numero inventado.
    u32 CapacidadeDeLote(u32 _PoolCount) {
        u32 Slots = 0;
        const u32 Motivo = GpuClassifyBatchBufferSlots(_PoolCount, Slots);
        Check(Motivo == GpuClassifyPreflightOk, "a porta de dimensionamento aprova a cena do teste");
        return Slots;
    }
    // ---- a cena do teste ----------------------------------------------------------------------
    //
    // Montada para que TODO desfecho seja alcancado por pelo menos um candidato, e para que dois
    // candidatos caiam no MESMO lote — sem isso, um shader que sobrescrevesse em vez de somar
    // passaria. Os DOIS pools recebem comando: e o que obriga cada lote a ter a sua regiao.
    constexpr u32 kPoolCount = 2u;

    std::vector<FGpuMaterialRecord> Materiais() {
        std::vector<FGpuMaterialRecord> M;
        MaterialConstants C{};
        M.push_back(FGpuMaterialRecord{});                                   // 0: one-sided opaco
        M.push_back(FGpuMaterialRecord::FromParts(C, true, false, 0u));      // 1: two-sided
        C.AlphaTest = 1u;
        M.push_back(FGpuMaterialRecord::FromParts(C, true, false, 0u));      // 2: masked
        C.AlphaTest = 0u;
        M.push_back(FGpuMaterialRecord::FromParts(C, true, true, 0u));       // 3: blend
        return M;
    }

    std::vector<FGpuMeshRecord> Malhas() {
        std::vector<FGpuMeshRecord> H(4);
        H[0].GeometryPoolId = 0u;
        H[1].GeometryPoolId = 1u;
        H[2].GeometryPoolId = 0u;
        H[2].Flags          = GpuMeshOffsetsUnrepresentable;
        H[3].GeometryPoolId = 7u; // fora da lista publicada: kPoolCount e 2
        // Geometria DISTINTA por malha e entre campos, para o scatter. Com tudo em zero, trocar
        // `IndexCountPerInstance` por `StartIndexLocation` no comando continuaria dando zero dos
        // dois lados. As contagens sao multiplas de 3 porque a tabela real marcaria as outras.
        for (u32 I = 0; I < 4u; ++I) {
            H[I].IndexCount   = 30u * (I + 1u);
            H[I].IndexOffset  = 100u * (I + 1u) + 7u;
            H[I].VertexOffset = static_cast<i32>(1000u * (I + 1u) + 13u);
        }
        return H;
    }

    // Dezessete linhas.
    //
    // As doze primeiras sao CANDIDATAS, uma por situacao. A generation e 1 em todas menos na 11, que
    // fica VAZIA.
    //
    // As cinco ultimas sao instancias VIVAS que NAO estao na lista de candidatos — uma escondida, uma
    // RaytracingOnly, e as demais como o culling da Fase 6 as deixaria de fora. Elas existem para dar
    // FOLGA as regioes: a capacidade vem das tabelas, e a contagem, da lista. Sem folga, capacidade e
    // contagem coincidiriam, e um offset tirado do prefixo das contagens passaria por este teste.
    std::vector<FGpuSceneInstanceMeta> Meta() {
        std::vector<FGpuSceneInstanceMeta> M(17);
        for (auto& R : M) { R.Generation = 1u; R.Flags = GpuSceneInstanceVisible; }
        const u32 Mat[17]  = { 0, 0, 0, 1, 2, 3, 0,  1, 0, 99, 0,  0,   0, 0, 0, 0, 1 };
        const u32 Mesh[17] = { 0, 1, 0, 0, 0, 0, 2,  2, 3,  0, 99, 0,   0, 1, 1, 0, 1 };
        for (u32 I = 0; I < 17u; ++I) { M[I].MaterialId = Mat[I]; M[I].MeshId = Mesh[I]; }
        M[11].Generation = 0u;                             // linha sem ocupante
        M[12].Flags      = 0u;                             // ESCONDIDA: viva, e fora da lista
        M[14].Flags     |= GpuSceneInstanceRaytracingOnly; // exclusao permanente do raster
        return M;
    }

    // Os catorze candidatos: as doze primeiras linhas, mais um buraco do expand e um slot fora da
    // tabela.
    std::vector<u32> Slots() {
        std::vector<u32> S;
        for (u32 I = 0; I < 12u; ++I) S.push_back(I);
        S.push_back(kGpuExpandNoSlot); // buraco deixado pelo expand
        S.push_back(999u);             // slot fora da tabela de meta
        return S;
    }


    // ---- parte CPU pura: a porta de DIMENSIONAMENTO -------------------------------------------
    //
    // Ela existe para ser o unico lugar onde a conversao para u32 e escrita. O teste fixa que ela
    // recusa ANTES de converter, e que uma recusa nao deixa numero plausivel na saida.
    void TestarPortaDeDimensionamento() {
        u32 Slots = 0xDEADBEEFu;
        CheckEq(GpuClassifyBatchBufferSlots(kPoolCount, Slots), GpuClassifyPreflightOk,
                "dois pools passam");
        CheckEq(Slots, kGpuPipelineClassCount * kPoolCount, "e devolvem quatro slots por pool");

        Slots = 0xDEADBEEFu;
        CheckEq(GpuClassifyBatchBufferSlots(0u, Slots), GpuClassifyPreflightNoPools,
                "sem pool nao ha dimensionamento");
        CheckEq(Slots, 0xDEADBEEFu, "recusa NAO escreve a saida");

        Slots = 0xDEADBEEFu;
        CheckEq(GpuClassifyBatchBufferSlots(UINT32_MAX / kGpuPipelineClassCount + 1u, Slots),
                GpuClassifyPreflightTooManyPools, "acima do limite recusa antes de converter");
        CheckEq(Slots, 0xDEADBEEFu, "e tambem nao escreve a saida");

        Slots = 0u;
        CheckEq(GpuClassifyBatchBufferSlots(UINT32_MAX / kGpuPipelineClassCount, Slots),
                GpuClassifyPreflightOk, "o ultimo valido passa");
        CheckEq(Slots, 4294967292u, "com o total exato, ja em u32");
    }

    // ---- parte CPU pura: a CAPACIDADE das regioes ---------------------------------------------
    //
    // Sai das TABELAS, e o oraculo e escrito a mao linha a linha. Cada exclusao tem uma linha propria
    // na cena, entao um criterio que contasse a mais ou a menos muda um numero aqui.
    //
    // A segunda metade fixa DE QUE a capacidade e funcao: associacao, classe e pool a mudam;
    // visibilidade nao. E o contrato que permite recalcula-la so quando uma daquelas tres muda.
    void TestarCapacidades() {
        const auto Mats  = Materiais(); const auto Meshs = Malhas();
        const auto Metas = Meta();      const auto S     = Slots();

        std::vector<u64> Cap;
        CheckEq(GpuCountRegionCapacities(Metas, Mats, Meshs, kPoolCount, Cap),
                GpuClassifyPreflightOk, "a cena do teste passa pela porta de lotes");
        CheckEq(static_cast<u32>(Cap.size()), CapacidadeDeLote(kPoolCount),
                "uma capacidade por slot de lote");
        //   lote 0 (classe 0, pool 0): linhas 0 e 2 (candidatas), 12 (ESCONDIDA) e 15 (fora da lista)
        //   lote 1 (classe 0, pool 1): linhas 1 (candidata) e 13 (fora da lista)
        //   fora de tudo: 3-5 e 16 (classes 1..3), 6 e 7 (malha barrada), 8-10 (canarios), 11 (vazia)
        //   e 14 (RaytracingOnly)
        const u64 Querido[8]{ 4ull, 2ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull };
        for (u32 B = 0; B < 8u && B < Cap.size(); ++B)
            CheckEq64(Cap[B], Querido[B], "capacidade do lote " + std::to_string(B));

        // NAO e a contagem do frame. Os mesmos lotes contam {2, 1} sobre a lista de candidatos; a
        // diferenca sao as instancias vivas que nao estao nela — exatamente o que o culling da Fase 6
        // vai produzir sem mexer nas regioes.
        FGpuClassifyInput In{};
        In.Slots = S; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
        In.PoolCount = kPoolCount;
        In.BatchCapacity = CapacidadeDeLote(kPoolCount);
        std::vector<u32> Lotes, Cont;
        ClassifyCandidates(In, Lotes, Cont);
        CheckEq(Lotes[0], 2u, "os candidatos contam 2 no lote 0, contra capacidade 4");
        CheckEq(Lotes[1], 1u, "e 1 no lote 1, contra capacidade 2");

        auto CapDe = [](const std::vector<FGpuSceneInstanceMeta>& _M,
                        const std::vector<FGpuMaterialRecord>& _Mat,
                        const std::vector<FGpuMeshRecord>& _Mesh, u32 _Lote) {
            std::vector<u64> C;
            GpuCountRegionCapacities(_M, _Mat, _Mesh, kPoolCount, C);
            return _Lote < C.size() ? C[_Lote] : ~0ull;
        };
        {
            // VISIBILIDADE nao muda: mostrar a escondida e esconder uma candidata mantem o lote 0 em 4.
            auto M = Metas;
            M[12].Flags = GpuSceneInstanceVisible;
            CheckEq64(CapDe(M, Mats, Meshs, 0u), 4ull, "mostrar a instancia escondida nao muda nada");
            M[0].Flags = 0u;
            CheckEq64(CapDe(M, Mats, Meshs, 0u), 4ull, "esconder uma candidata tambem nao");
        }
        {
            // A flag RaytracingOnly e o que tira a linha 14: sem ela, o lote 1 ganha uma.
            auto M = Metas;
            M[14].Flags = GpuSceneInstanceVisible;
            CheckEq64(CapDe(M, Mats, Meshs, 1u), 3ull, "sem RaytracingOnly a linha 14 conta");
        }
        {
            // ASSOCIACAO muda: a linha 13 trocando para a malha 0 passa do lote 1 para o lote 0.
            auto M = Metas;
            M[13].MeshId = 0u;
            CheckEq64(CapDe(M, Mats, Meshs, 0u), 5ull, "associacao: a linha 13 entra no lote 0");
            CheckEq64(CapDe(M, Mats, Meshs, 1u), 1ull, "e sai do lote 1");
        }
        {
            // POOL muda: a malha 1 indo para o pool 0 leva o lote 1 inteiro junto.
            auto H = Meshs;
            H[1].GeometryPoolId = 0u;
            CheckEq64(CapDe(Metas, Mats, H, 0u), 6ull, "pool: a malha 1 no pool 0 junta os dois lotes");
            CheckEq64(CapDe(Metas, Mats, H, 1u), 0ull, "e o lote 1 fica vazio");
        }
        {
            // CLASSE muda: o material 0 virando masked tira as seis instancias do indireto.
            auto Mt = Mats;
            MaterialConstants C{};
            C.AlphaTest = 1u;
            Mt[0] = FGpuMaterialRecord::FromParts(C, false, false, 0u);
            CheckEq64(CapDe(Metas, Mt, Meshs, 0u), 0ull, "classe: material masked sai do lote 0");
            CheckEq64(CapDe(Metas, Mt, Meshs, 1u), 0ull, "e do lote 1");
        }
        {
            // A porta de lotes recusa sem escrever.
            std::vector<u64> Sentinela{ 77ull };
            CheckEq(GpuCountRegionCapacities(Metas, Mats, Meshs, 0u, Sentinela),
                    GpuClassifyPreflightNoPools, "sem pool nao ha capacidade");
            Check(Sentinela.size() == 1 && Sentinela[0] == 77ull, "e a recusa nao escreve a saida");
            CheckEq(GpuCountRegionCapacities(Metas, Mats, Meshs,
                                             UINT32_MAX / kGpuPipelineClassCount + 1u, Sentinela),
                    GpuClassifyPreflightTooManyPools, "pools demais tambem recusam");
            Check(Sentinela.size() == 1 && Sentinela[0] == 77ull, "e tambem nao escrevem");
        }
    }

    // ---- parte CPU pura: o LAYOUT e a porta de u32 --------------------------------------------
    //
    // Regioes densas na ordem dos lotes. A soma corre em u64 e so vira u32 depois da porta; os casos
    // de fronteira abaixo sao os que um layout somado em u32 aprovaria com regioes plausiveis.
    void TestarLayout() {
        auto Igual = [](const FGpuIndirectRegion& _R, u32 _Offset, u32 _Capacity) {
            return _R.Offset == _Offset && _R.Capacity == _Capacity;
        };
        {
            // O layout da cena do teste, com as capacidades que TestarCapacidades fixou.
            const u64 Cap[8]{ 4ull, 2ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull };
            std::vector<FGpuIndirectRegion> R;
            u32 Total = 0xDEADBEEFu;
            CheckEq(GpuLayoutRegions(Cap, R, Total), GpuRegionLayoutOk, "o layout da cena passa");
            CheckEq(Total, 6u, "seis comandos no total");
            Check(R.size() == 8, "uma regiao por slot de lote");
            if (R.size() == 8) {
                Check(Igual(R[0], 0u, 4u), "regiao 0 = [0, 4)");
                Check(Igual(R[1], 4u, 2u), "regiao 1 = [4, 6): comeca DEPOIS da folga do lote 0");
                for (u32 B = 2; B < 8u; ++B)
                    Check(Igual(R[B], 6u, 0u),
                          "regiao vazia " + std::to_string(B) + " fica no fim, sem espaco");
            }
        }
        {
            // Lotes VAZIOS entre cheios nao consomem espaco: o offset do vazio e o do seguinte.
            const u64 Cap[5]{ 2ull, 0ull, 5ull, 0ull, 3ull };
            const u32 Offsets[5]{ 0u, 2u, 2u, 7u, 7u };
            std::vector<FGpuIndirectRegion> R;
            u32 Total = 0u;
            CheckEq(GpuLayoutRegions(Cap, R, Total), GpuRegionLayoutOk,
                    "lotes vazios intercalados passam");
            CheckEq(Total, 10u, "o total e a soma");
            for (u32 B = 0; B < 5u && B < R.size(); ++B)
                CheckEq(R[B].Offset, Offsets[B], "offset denso do lote " + std::to_string(B));
        }
        {
            // A fronteira: soma EXATAMENTE UINT32_MAX passa, e as regioes saem certas.
            const u64 Cap[2]{ 0xFFFFFFFEull, 1ull };
            std::vector<FGpuIndirectRegion> R;
            u32 Total = 0u;
            CheckEq(GpuLayoutRegions(Cap, R, Total), GpuRegionLayoutOk, "soma igual a UINT32_MAX passa");
            CheckEq(Total, 0xFFFFFFFFu, "com o total exato");
            Check(R.size() == 2 && Igual(R[1], 0xFFFFFFFEu, 1u),
                  "e a ultima regiao comeca no ultimo comando");
        }
        {
            // Um acima recusa, e a recusa nao escreve nada.
            const u64 Cap[2]{ 0xFFFFFFFFull, 1ull };
            std::vector<FGpuIndirectRegion> R{ FGpuIndirectRegion{ 7u, 7u } };
            u32 Total = 0xDEADBEEFu;
            CheckEq(GpuLayoutRegions(Cap, R, Total), GpuRegionLayoutTooManyCommands,
                    "um acima de UINT32_MAX recusa");
            Check(R.size() == 1 && Igual(R[0], 7u, 7u), "e a recusa nao escreve as regioes");
            CheckEq(Total, 0xDEADBEEFu, "nem o total");
        }
        {
            // A soma que em u32 daria a volta para ZERO. Um layout em u32 a aprovaria com total 0 e
            // poria a regiao 1 no offset 0x80000000 — plausivel, e dentro de nada.
            const u64 Cap[2]{ 0x80000000ull, 0x80000000ull };
            std::vector<FGpuIndirectRegion> R;
            u32 Total = 0u;
            CheckEq(GpuLayoutRegions(Cap, R, Total), GpuRegionLayoutTooManyCommands,
                    "a soma que daria a volta para zero recusa");
        }
        {
            // Uma capacidade SOZINHA acima de u32: truncada, viraria zero e o lote sumiria do layout.
            const u64 Cap[1]{ 0x100000000ull };
            std::vector<FGpuIndirectRegion> R;
            u32 Total = 0u;
            CheckEq(GpuLayoutRegions(Cap, R, Total), GpuRegionLayoutTooManyCommands,
                    "uma capacidade acima de u32 recusa");
        }
        {
            // A soma que da a volta em U64. `{UINT64_MAX, 1}` somaria zero e passaria como layout
            // vazio; `{1, UINT64_MAX}` tambem, com a regiao do segundo lote truncada. As duas ordens,
            // porque cada uma prova um termo da porta: sem o limite por parcela, a segunda passa.
            const u64 Casos[2][2]{ { UINT64_MAX, 1ull }, { 1ull, UINT64_MAX } };
            for (u32 K = 0; K < 2u; ++K) {
                const std::string N = K == 0u ? "{UINT64_MAX, 1}" : "{1, UINT64_MAX}";
                std::vector<FGpuIndirectRegion> R{ FGpuIndirectRegion{ 7u, 7u } };
                u32 Total = 0xDEADBEEFu;
                CheckEq(GpuLayoutRegions(Casos[K], R, Total), GpuRegionLayoutTooManyCommands,
                        N + ": a soma que da a volta em u64 recusa");
                Check(R.size() == 1 && Igual(R[0], 7u, 7u), N + ": e a recusa nao escreve as regioes");
                CheckEq(Total, 0xDEADBEEFu, N + ": nem o total");
            }
        }
        {
            // Nenhum lote: nenhuma regiao, total zero.
            std::vector<FGpuIndirectRegion> R{ FGpuIndirectRegion{ 7u, 7u } };
            u32 Total = 9u;
            CheckEq(GpuLayoutRegions({}, R, Total), GpuRegionLayoutOk, "sem lote o layout e vazio");
            Check(R.empty(), "e a saida fica vazia, e nao com o conteudo anterior");
            CheckEq(Total, 0u, "com total zero");
        }

        // Os dois parametros que a gravacao tira da regiao, em u64.
        CheckEq64(GpuRegionArgumentOffset(FGpuIndirectRegion{ 4u, 2u }), 112ull,
                  "ArgumentBufferOffset = Offset * 28");
        CheckEq64(GpuRegionArgumentOffset(FGpuIndirectRegion{ 0xFFFFFFFFu, 1u }),
                  0xFFFFFFFFull * 28ull, "o maior offset nao da a volta");
        CheckEq64(GpuRegionCountOffset(1u), 4ull, "CountBufferOffset = lote * 4");
        CheckEq64(GpuRegionCountOffset(0xFFFFFFFFu), 0xFFFFFFFFull * 4ull,
                  "o maior lote nao da a volta");
    }

    // ---- parte CPU pura: a porta das REGIOES ---------------------------------------------------
    //
    // Estrutura, e nao suficiencia: ela confere que as regioes cobrem todo lote, cabem no recurso e
    // nao se sobrepoem. Se a contagem do FRAME cabe em cada regiao ela nao tem como saber — isso e
    // garantido pela capacidade estavel e verificado pela GPU.
    void TestarPreflightDeRegioes() {
        const FGpuIndirectRegion Cena[8]{ { 0u, 4u }, { 4u, 2u }, { 6u, 0u }, { 6u, 0u },
                                          { 6u, 0u }, { 6u, 0u }, { 6u, 0u }, { 6u, 0u } };
        CheckEq(GpuRegionPreflight(Cena, 8u, 6u), GpuRegionPreflightOk,
                "as regioes da cena passam com a capacidade exata");
        CheckEq(GpuRegionPreflight(Cena, 8u, 5u), GpuRegionPreflightSmallArgsBuffer,
                "um comando a menos reprova");
        CheckEq(GpuRegionPreflight(std::span<const FGpuIndirectRegion>(Cena, 7u), 8u, 6u),
                GpuRegionPreflightSmallRegionTable, "uma regiao a menos que os lotes reprova");

        FGpuIndirectRegion Sobre[8];
        std::copy(std::begin(Cena), std::end(Cena), Sobre);
        Sobre[1].Offset = 3u; // [3, 5): a ultima posicao do lote 0 e tambem a primeira do lote 1
        CheckEq(GpuRegionPreflight(Sobre, 8u, 6u), GpuRegionPreflightOverlap,
                "regioes sobrepostas reprovam, mesmo cabendo no recurso");

        FGpuIndirectRegion Invertida[8];
        std::copy(std::begin(Cena), std::end(Cena), Invertida);
        Invertida[0] = { 2u, 4u };
        Invertida[1] = { 0u, 2u };
        CheckEq(GpuRegionPreflight(Invertida, 8u, 6u), GpuRegionPreflightOverlap,
                "regioes fora da ordem dos lotes reprovam");

        // O fim que em u32 daria a volta: `0xFFFFFFFF + 2` seria 1, e 1 cabe em qualquer recurso.
        const FGpuIndirectRegion Volta[1]{ { 0xFFFFFFFFu, 2u } };
        CheckEq(GpuRegionPreflight(Volta, 1u, 0xFFFFFFFFu), GpuRegionPreflightSmallArgsBuffer,
                "o fim de regiao que daria a volta reprova");

        // E a garantia de producao: o layout, contra o total que ele mesmo devolve, sempre passa.
        const u64 Cap[5]{ 2ull, 0ull, 5ull, 0ull, 3ull };
        std::vector<FGpuIndirectRegion> R;
        u32 Total = 0u;
        GpuLayoutRegions(Cap, R, Total);
        CheckEq(GpuRegionPreflight(R, 5u, Total), GpuRegionPreflightOk,
                "o layout de producao passa com o proprio total");
        CheckEq(GpuRegionPreflight(R, 5u, Total - 1u), GpuRegionPreflightSmallArgsBuffer,
                "e reprova com um comando a menos");
    }

    // ---- parte CPU pura: o desfecho de cada candidato -----------------------------------------
    //
    // O oraculo e uma TABELA escrita a mao, e nao a funcao rodada de novo: a referencia CPU e o
    // shader sao dois espelhos do mesmo raciocinio, e compara-los entre si nao pegaria um raciocinio
    // errado nos dois.
    void TestarDesfechos() {
        const auto Mats  = Materiais();
        const auto Meshs = Malhas();
        const auto Metas = Meta();
        const auto S     = Slots();

        const u32 kEsperado[14] = {
            GpuClassifyIndirect,          // 0: one-sided, pool 0
            GpuClassifyIndirect,          // 1: one-sided, pool 1
            GpuClassifyIndirect,          // 2: one-sided, pool 0 de novo (mesmo lote que o 0)
            GpuClassifyNotEligible,       // 3: two-sided
            GpuClassifyNotEligible,       // 4: masked
            GpuClassifyNotEligible,       // 5: blend
            GpuClassifyMeshUnusable,   // 6: classe elegivel, malha com a flag
            GpuClassifyNotEligible,       // 7: os DOIS termos fechados -> a classe tem precedencia
            GpuClassifyInvalidPoolId,     // 8: pool fora da lista
            GpuClassifyInvalidMaterialId, // 9
            GpuClassifyInvalidMeshId,     // 10
            GpuClassifyInvalidSlot,       // 11: linha sem ocupante
            GpuClassifyNoSlot,            // 12: buraco do expand
            GpuClassifyInvalidSlot,       // 13: slot fora da tabela
        };
        for (u32 I = 0; I < 14u; ++I) {
            FGpuClassifyCommand Key{};
            const u32 Got = GpuClassifyCandidate(S[I], Metas, Mats, Meshs, kPoolCount, Key);
            Check(Got == kEsperado[I],
                  std::string("candidato ") + std::to_string(I) + ": desfecho " +
                      NomeDesfecho(Got) + ", esperado " + NomeDesfecho(kEsperado[I]));
        }
    }

    // ---- parte CPU pura: o indice do lote -----------------------------------------------------
    void TestarIndiceDeLote() {
        CheckEq(CapacidadeDeLote(kPoolCount), kGpuPipelineClassCount * kPoolCount,
                "a tabela de lotes reserva as quatro classes");
        // CLASS-MAJOR: os lotes de uma classe sao contiguos, e classes diferentes nunca colidem.
        CheckEq(GpuBatchIndex({ GpuPipelineClassOpaqueOneSided, 0u }, kPoolCount), 0u, "lote {0,0}");
        CheckEq(GpuBatchIndex({ GpuPipelineClassOpaqueOneSided, 1u }, kPoolCount), 1u, "lote {0,1}");
        CheckEq(GpuBatchIndex({ GpuPipelineClassOpaqueTwoSided, 0u }, kPoolCount), 2u, "lote {1,0}");
        CheckEq(GpuBatchIndex({ GpuPipelineClassBlend, 1u }, kPoolCount), 7u, "lote {3,1}");
        // Todos os pares distintos dao indices distintos e dentro da capacidade. Um indice
        // pool-major com class-major misturados colidiria aqui.
        std::vector<u32> Vistos;
        for (u32 C = 0; C < kGpuPipelineClassCount; ++C)
            for (u32 P = 0; P < kPoolCount; ++P) {
                const u32 Idx = GpuBatchIndex({ C, P }, kPoolCount);
                Check(Idx < GpuBatchSlotCount(kPoolCount), "indice de lote dentro da capacidade");
                Vistos.push_back(Idx);
            }
        std::sort(Vistos.begin(), Vistos.end());
        Check(std::unique(Vistos.begin(), Vistos.end()) == Vistos.end(),
              "pares distintos dao indices distintos");
    }
    // ---- parte CPU pura: UM RESULTADO DE CLASSIFICACAO por candidato --------------------------
    //
    // A soma dos nove contadores tem de dar o numero de candidatos. Isso pega um caminho que esquece
    // de contar e um que conta duas vezes — e so isso.
    //
    // Nao confundir com "desenhado exatamente uma vez": esse e outro invariante, sobre outra
    // particao, e nao existe enquanto o fallback e o scatter nao existirem. Ver TestarParticoes logo
    // abaixo, que registra por que as duas nao coincidem.
    void TestarUmResultadoPorCandidato() {
        const auto Mats = Materiais(); const auto Meshs = Malhas();
        const auto Metas = Meta();     const auto S = Slots();

        FGpuClassifyInput In{};
        In.Slots = S; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
        In.PoolCount = kPoolCount;
        In.BatchCapacity = CapacidadeDeLote(kPoolCount);

        std::vector<u32> Lotes, Contadores;
        const u32 Indiretos = ClassifyCandidates(In, Lotes, Contadores);

        u32 Soma = 0;
        for (const u32 C : Contadores) Soma += C;
        CheckEq(Soma, static_cast<u32>(S.size()),
                "todo candidato recebe exatamente um resultado de classificacao");
        CheckEq(Indiretos, Contadores[GpuClassifyIndirect], "o retorno e o contador de indiretos");

        u32 SomaLotes = 0;
        for (const u32 L : Lotes) SomaLotes += L;
        CheckEq(SomaLotes, Contadores[GpuClassifyIndirect],
                "a soma dos lotes e o numero de comandos");
    }

    // ---- as duas particoes NAO coincidem -------------------------------------------------------
    //
    // Contraexemplo escrito como teste para nao viver so num comentario:
    //
    //     a CPU desenha:  !GpuIndirectEligible(classe, flags)
    //     a GPU emite:     GpuIndirectEligible(classe, flags) E ids validos
    //
    // Logo o conjunto EXPOSTO — que ninguem desenha — e `elegivel E canario`. A mesma malha de pool
    // invalido sob material masked e desenhada uma vez (por acaso, nao porque as contas fecharam);
    // sob material opaco elegivel, nenhuma vez.
    void TestarParticoes() {
        const auto Mats = Materiais(); const auto Meshs = Malhas();

        FGpuClassifyCommand K{};
        FGpuSceneInstanceMeta Masked{};
        Masked.Generation = 1u; Masked.MaterialId = 2u; Masked.MeshId = 3u;
        FGpuSceneInstanceMeta Opaco{};
        Opaco.Generation = 1u;  Opaco.MaterialId = 0u;  Opaco.MeshId = 3u;
        const std::vector<FGpuSceneInstanceMeta> Par{ Masked, Opaco };

        CheckEq(GpuClassifyCandidate(0u, Par, Mats, Meshs, kPoolCount, K),
                GpuClassifyInvalidPoolId, "masked com pool invalido: canario");
        CheckEq(GpuClassifyCandidate(1u, Par, Mats, Meshs, kPoolCount, K),
                GpuClassifyInvalidPoolId, "opaco com pool invalido: o MESMO canario");

        // Mesmo desfecho, destinos diferentes na CPU. E esse o ponto.
        Check(!GpuIndirectEligible(Mats[2].PipelineClass, Meshs[3].Flags),
              "o masked cai no complemento da CPU, e e desenhado uma vez");
        Check(GpuIndirectEligible(Mats[0].PipelineClass, Meshs[3].Flags),
              "o opaco NAO cai no complemento: ninguem o desenha");

        // O conjunto exposto da cena do teste, calculado como a definicao manda. Nao e zero aqui
        // porque a cena foi montada com defeitos de proposito; numa cena sa ele tem de ser zero, e e
        // isso que os canarios diagnosticam.
        const auto Metas = Meta(); const auto S = Slots();
        u32 Expostos = 0, Incognitos = 0;
        for (const u32 Slot : S) {
            FGpuClassifyCommand Key{};
            const u32 O = GpuClassifyCandidate(Slot, Metas, Mats, Meshs, kPoolCount, Key);
            // Politica: alguem desenha, e nao ha exposicao.
            if (O == GpuClassifyIndirect || O == GpuClassifyNotEligible ||
                O == GpuClassifyMeshUnusable)
                continue;
            // Canario. A classe so e conhecida quando slot e os dois ids sao validos — sem ela nao
            // da nem para dizer se o candidato estava exposto, que e o pior caso.
            if (Slot >= Metas.size()) { ++Incognitos; continue; }
            const FGpuSceneInstanceMeta& M = Metas[Slot];
            if (M.Generation == 0u || M.MaterialId >= Mats.size() || M.MeshId >= Meshs.size()) {
                ++Incognitos;
                continue;
            }
            if (GpuIndirectEligible(Mats[M.MaterialId].PipelineClass, Meshs[M.MeshId].Flags))
                ++Expostos;
        }
        CheckEq(Expostos, 1u, "um candidato exposto: classe elegivel sobre pool invalido");
        CheckEq(Incognitos, 5u, "e cinco canarios em que a classe nem chega a ser conhecida");
    }

    // ---- o preflight: a porta do FRAME ---------------------------------------------------------
    //
    // Tudo que ele olha e conhecido pela CPU antes do dispatch. Nenhum motivo depende de contador que
    // a GPU escreveu — se dependesse, seria readback decidindo draw, que a regra 7 proibe.
    void TestarPreflight() {
        const auto Mats = Materiais(); const auto Meshs = Malhas();
        const auto Metas = Meta();     const auto S = Slots();

        FGpuClassifyInput In{};
        In.Slots = S; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
        In.PoolCount = kPoolCount;
        In.BatchCapacity = CapacidadeDeLote(kPoolCount);
        CheckEq(GpuClassifyPreflight(In), GpuClassifyPreflightOk, "a cena do teste passa");

        // A CAPACIDADE EXATA passa, e uma abaixo dela nao. E este limite que torna o overflow
        // inalcancavel: nao existe folga a descobrir depois do dispatch.
        FGpuClassifyInput Curta = In;
        Curta.BatchCapacity = CapacidadeDeLote(kPoolCount) - 1u;
        CheckEq(GpuClassifyPreflight(Curta), GpuClassifyPreflightSmallBatchBuffer,
                "um slot a menos ja reprova");
        Curta.BatchCapacity = 1u;
        CheckEq(GpuClassifyPreflight(Curta), GpuClassifyPreflightSmallBatchBuffer,
                "a capacidade que o teste usa para exercitar o overflow e, ela propria, reprovada");

        FGpuClassifyInput SemPool = In;
        SemPool.PoolCount = 0u;
        CheckEq(GpuClassifyPreflight(SemPool), GpuClassifyPreflightNoPools, "sem pool, reprova");

        // ---- a fronteira em que o produto nao cabe mais em u32 --------------------------------
        //
        // `kGpuPipelineClassCount * PoolCount` em u32 da a volta a partir de 2^30, e da a volta para
        // ZERO: sem o calculo em u64, uma capacidade qualquer passaria e o overflow que o preflight
        // promete eliminar voltaria a ser alcancavel.
        {
            constexpr u32 kUltimoValido = UINT32_MAX / kGpuPipelineClassCount; // 2^30 - 1
            constexpr u32 kPrimeiroInvalido = kUltimoValido + 1u;              // 2^30

            // O produto exato, sem truncar. Em u32 o segundo daria ZERO.
            CheckEq64(GpuBatchSlotCount(kUltimoValido), 4294967292ull,
                      "o ultimo PoolCount valido pede 4.294.967.292 slots");
            CheckEq64(GpuBatchSlotCount(kPrimeiroInvalido), 4294967296ull,
                      "o primeiro invalido pede 2^32 — que em u32 seria zero");

            FGpuClassifyInput Lim = In;
            Lim.PoolCount     = kUltimoValido;
            Lim.BatchCapacity = 4294967292u;
            CheckEq(GpuClassifyPreflight(Lim), GpuClassifyPreflightOk,
                    "a ultima entrada valida passa, com a capacidade exata");
            Lim.BatchCapacity = 4294967291u;
            CheckEq(GpuClassifyPreflight(Lim), GpuClassifyPreflightSmallBatchBuffer,
                    "e um slot abaixo dela reprova por buffer pequeno");

            Lim.PoolCount     = kPrimeiroInvalido;
            Lim.BatchCapacity = UINT32_MAX;
            CheckEq(GpuClassifyPreflight(Lim), GpuClassifyPreflightTooManyPools,
                    "o primeiro invalido reprova mesmo com a maior capacidade que existe");
            // E o caso que o produto truncado deixaria passar: capacidade ZERO contra um necessario
            // que, em u32, tambem seria zero.
            Lim.BatchCapacity = 0u;
            CheckEq(GpuClassifyPreflight(Lim), GpuClassifyPreflightTooManyPools,
                    "e com capacidade zero, que e o que o produto truncado aprovaria");

            // O indice continua cabendo em u32 na ultima entrada valida: o maior possivel e
            // exatamente `GpuBatchSlotCount - 1`.
            const u32 Maior = GpuBatchIndex({ kGpuPipelineClassCount - 1u, kUltimoValido - 1u },
                                            kUltimoValido);
            CheckEq(Maior, 4294967291u, "o maior indice sob preflight aprovado nao da a volta");
        }

        for (int Qual = 0; Qual < 3; ++Qual) {
            FGpuClassifyInput Vazia = In;
            if (Qual == 0) Vazia.Meta = {};
            if (Qual == 1) Vazia.Materials = {};
            if (Qual == 2) Vazia.Meshes = {};
            CheckEq(GpuClassifyPreflight(Vazia), GpuClassifyPreflightEmptyTable,
                    "tabela vazia com candidatos na fila reprova (" + std::to_string(Qual) + ")");
        }

        // Cena sem candidato NAO e falha: um dispatch de zero threads e valido e nao produz desfecho.
        FGpuClassifyInput SemCandidato = In;
        SemCandidato.Slots = {};
        SemCandidato.PoolCount = 0u;
        CheckEq(GpuClassifyPreflight(SemCandidato), GpuClassifyPreflightOk,
                "cena sem candidato passa, mesmo sem pool");

        // E a consequencia que interessa: sob preflight aprovado, o overflow nao acontece.
        std::vector<u32> Lotes, Cont;
        ClassifyCandidates(In, Lotes, Cont);
        CheckEq(Cont[GpuClassifyOverflow], 0u, "preflight aprovado: overflow inalcancavel");
    }

    // ---- o COMANDO que a classificacao produz --------------------------------------------------
    //
    // Cada campo tem UMA fonte possivel, e o oraculo e a propria linha de origem — nao a funcao rodada
    // de novo. A malha do teste tem geometria distinta entre si e entre campos, entao uma troca de
    // dois campos no comando nao tem como bater por coincidencia.
    void TestarComando() {
        const auto Mats = Materiais(); const auto Meshs = Malhas();
        const auto Metas = Meta();

        // Slot 1: material 0, malha 1 (pool 1).
        FGpuClassifyCommand C{};
        CheckEq(GpuClassifyCandidate(1u, Metas, Mats, Meshs, kPoolCount, C), GpuClassifyIndirect,
                "o slot 1 e indireto");
        CheckEq(C.Batch, GpuBatchIndex({ GpuPipelineClassOpaqueOneSided, 1u }, kPoolCount),
                "lote = classe 0, pool 1");
        CheckEq(C.Args.MaterialRow, Metas[1].MaterialId, "MaterialRow vem do MaterialId da meta");
        CheckEq(C.Args.InstanceSlot, 1u, "InstanceSlot e o proprio slot do candidato");
        CheckEq(C.Args.IndexCountPerInstance, Meshs[1].IndexCount, "contagem vem da malha");
        CheckEq(C.Args.InstanceCount, 1u, "uma instancia por comando");
        CheckEq(C.Args.StartIndexLocation, Meshs[1].IndexOffset, "inicio do indice vem da malha");
        Check(C.Args.BaseVertexLocation == Meshs[1].VertexOffset, "base do vertice vem da malha");
        CheckEq(C.Args.StartInstanceLocation, 0u, "StartInstanceLocation fica em zero");

        // Recusa nao escreve o comando: um candidato recusado nao pode deixar argumento plausivel.
        FGpuClassifyCommand R{};
        R.Batch = 0xABCDu;
        R.Args.IndexCountPerInstance = 0xABCDu;
        CheckEq(GpuClassifyCandidate(3u, Metas, Mats, Meshs, kPoolCount, R),
                GpuClassifyNotEligible, "o slot 3 e recusado");
        CheckEq(R.Batch, 0xABCDu, "e a recusa nao toca no lote");
        CheckEq(R.Args.IndexCountPerInstance, 0xABCDu, "nem no comando");
    }

    // ---- ferramentas do scatter ----------------------------------------------------------------

    // O comando POISON que semeia `IndirectArgs`. Todo campo em 0xA5A5A5A5: nenhum comando real da
    // cena do teste tem esse valor em campo nenhum, entao uma posicao que o scatter deixou de escrever
    // aparece como poison, e nunca como um comando plausivel.
    FGpuIndirectDrawArgs ComandoPoison() {
        FGpuIndirectDrawArgs P{};
        P.MaterialRow = P.InstanceSlot = P.IndexCountPerInstance = P.InstanceCount = 0xA5A5A5A5u;
        P.StartIndexLocation = P.StartInstanceLocation = 0xA5A5A5A5u;
        P.BaseVertexLocation = static_cast<i32>(0xA5A5A5A5u);
        return P;
    }

    bool MesmoComando(const FGpuIndirectDrawArgs& A, const FGpuIndirectDrawArgs& B) {
        return std::memcmp(&A, &B, sizeof(FGpuIndirectDrawArgs)) == 0;
    }

    // Ordena um trecho de comandos pelos BYTES. Serve so para comparar multiconjuntos: a ordem dentro
    // de um lote nao tem significado, e ordenar os dois lados pelo mesmo criterio a elimina.
    std::vector<FGpuIndirectDrawArgs> Ordenados(const FGpuIndirectDrawArgs* _Inicio, u32 _N) {
        std::vector<FGpuIndirectDrawArgs> V(_Inicio, _Inicio + _N);
        std::sort(V.begin(), V.end(), [](const FGpuIndirectDrawArgs& A, const FGpuIndirectDrawArgs& B) {
            return std::memcmp(&A, &B, sizeof(FGpuIndirectDrawArgs)) < 0;
        });
        return V;
    }

    // O comando esperado, escrito A MAO a partir da cena do teste — e nao pela funcao que produz o
    // comando, que e o que esta sob teste.
    FGpuIndirectDrawArgs Esperado(u32 _Slot, u32 _Malha) {
        FGpuIndirectDrawArgs E{};
        E.MaterialRow           = 0u;     // os tres candidatos indiretos usam o material 0
        E.InstanceSlot          = _Slot;
        E.IndexCountPerInstance = 30u * (_Malha + 1u);
        E.InstanceCount         = 1u;
        E.StartIndexLocation    = 100u * (_Malha + 1u) + 7u;
        E.BaseVertexLocation    = static_cast<i32>(1000u * (_Malha + 1u) + 13u);
        E.StartInstanceLocation = 0u;
        return E;
    }

    // O conteudo de `IndirectArgs` sob as regioes da cena, {0,4} e {4,2}, depois de UMA execucao:
    //
    //   [0, 2)  lote 0: slots 0 e 2 na malha 0, em qualquer ordem   -> multiconjunto
    //   [2, 4)  FOLGA do lote 0                                      -> poison
    //   [4, 5)  lote 1: slot 1 na malha 1                            -> comando exato
    //   [5, 6)  FOLGA do lote 1                                      -> poison
    //
    // A folga continuar poison e o que prova que cada comando foi para `Offset + cursor` da regiao do
    // SEU lote: um scatter que ignorasse o offset poria o slot 1 na posicao 0, e um que usasse o
    // prefixo das contagens o poria na posicao 2 — dentro da folga do lote 0.
    void ConferirArgsDaCena(std::span<const FGpuIndirectDrawArgs> _Args, const std::string& _Quem) {
        Check(_Args.size() >= 6, _Quem + ": IndirectArgs cobre as seis posicoes das regioes");
        if (_Args.size() < 6) return;
        const FGpuIndirectDrawArgs L0[2]{ Esperado(0u, 0u), Esperado(2u, 0u) };
        const auto Got0  = Ordenados(_Args.data(), 2u);
        const auto Want0 = Ordenados(L0, 2u);
        Check(MesmoComando(Got0[0], Want0[0]) && MesmoComando(Got0[1], Want0[1]),
              _Quem + ": regiao 0, o multiconjunto exato");
        Check(MesmoComando(_Args[4], Esperado(1u, 1u)), _Quem + ": regiao 1, o comando exato");
        for (const u32 P : { 2u, 3u, 5u })
            Check(MesmoComando(_Args[P], ComandoPoison()),
                  _Quem + ": folga na posicao " + std::to_string(P) + " continua poison");
    }

    // ---- parte CPU pura: a referencia do scatter, dentro das regioes -------------------------
    void TestarScatterCpu() {
        // Bytes em u64. Em u32, 153.391.690 comandos ja dariam a volta.
        CheckEq64(GpuScatterArgsBytes(6u), 6ull * 28ull, "seis comandos, 28 B cada");
        CheckEq64(GpuScatterArgsBytes(0u), 28ull, "zero comandos ainda aloca um: recurso vazio e ilegal");
        CheckEq64(GpuScatterArgsBytes(0xFFFFFFFFu), 0xFFFFFFFFull * 28ull,
                  "o maior u32 de comandos nao da a volta nos bytes");

        const auto Mats = Materiais(); const auto Meshs = Malhas();
        const auto Metas = Meta();     const auto S = Slots();
        FGpuClassifyInput In{};
        In.Slots = S; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
        In.PoolCount = kPoolCount;
        In.BatchCapacity = CapacidadeDeLote(kPoolCount);

        std::vector<u32> Lotes, ContClassify;
        ClassifyCandidates(In, Lotes, ContClassify);

        // As regioes pela funcao de PRODUCAO, das tabelas.
        std::vector<u64> Cap;
        std::vector<FGpuIndirectRegion> Reg;
        u32 Total = 0u;
        GpuCountRegionCapacities(Metas, Mats, Meshs, kPoolCount, Cap);
        GpuLayoutRegions(Cap, Reg, Total);

        std::vector<FGpuIndirectDrawArgs> Args(Total, ComandoPoison());
        std::vector<u32> Cont;
        std::vector<u32> Cursores(Lotes.size(), 0u);
        ScatterIndirectArgs(In, Reg, Lotes, Cursores, Args, Cont);

        // A contagem e a emissao olham o mesmo fato. Um scatter que recusasse alguem que o classify
        // contou apareceria AQUI, antes de virar comando velho no ExecuteIndirect.
        CheckEq(Cont[GpuScatterCounterEmitted], ContClassify[GpuClassifyIndirect],
                "o scatter emite exatamente o que o classify contou");
        CheckEq(Cont[GpuScatterCounterFaultCursor], 0u, "cursores zerados: nenhuma falha de cursor");
        CheckEq(Cont[GpuScatterCounterFaultRegion], 0u, "regioes das tabelas: nenhum lote passa da sua");
        CheckEq(Cont[GpuScatterCounterFaultCapacity], 0u, "regioes no recurso: nenhuma falha de capacidade");
        ConferirArgsDaCena(Args, "CPU");

        // REGIAO CURTA: o lote 0 tem dois comandos e uma posicao. O excedente vira falha, e a posicao
        // seguinte — que e do lote 1 — fica com o comando do lote 1.
        std::vector<FGpuIndirectRegion> Curta(In.BatchCapacity, FGpuIndirectRegion{ 3u, 0u });
        Curta[0] = { 0u, 1u };
        Curta[1] = { 1u, 2u };
        std::vector<FGpuIndirectDrawArgs> ArgsC(3u, ComandoPoison());
        std::vector<u32> CurC(Lotes.size(), 0u), ContC;
        ScatterIndirectArgs(In, Curta, Lotes, CurC, ArgsC, ContC);
        CheckEq(ContC[GpuScatterCounterFaultRegion], 1u, "CPU, regiao curta: o excedente vira falha");
        CheckEq(ContC[GpuScatterCounterEmitted], 2u, "CPU, regiao curta: os outros dois sao escritos");
        Check(MesmoComando(ArgsC[0], Esperado(0u, 0u)),
              "CPU, regiao curta: a posicao do lote 0 fica com o primeiro candidato dele");
        Check(MesmoComando(ArgsC[1], Esperado(1u, 1u)),
              "CPU, regiao curta: a posicao 1, do lote 1, nao e invadida");
        Check(MesmoComando(ArgsC[2], ComandoPoison()), "CPU, regiao curta: a folga do lote 1 fica poison");
    }
}

namespace {
    struct FResultado {
        std::vector<u32> Lotes;
        std::vector<u32> Contadores;
    };

    // A sonda. Mesma forma da do expand; o que muda sao as views e o CB.
    struct FSonda {
        wrl::ComPtr<ID3D12Device>        Device;
        wrl::ComPtr<ID3D12CommandQueue>  Queue;
        wrl::ComPtr<ID3D12RootSignature> Root;
        wrl::ComPtr<ID3D12RootSignature> RootScatter;
        wrl::ComPtr<ID3D12RootSignature> RootFinalize;
        wrl::ComPtr<ID3D12PipelineState> Pipeline;
        wrl::ComPtr<ID3D12PipelineState> PipelineScatter;
        wrl::ComPtr<ID3D12PipelineState> PipelineFinalize;
        wrl::ComPtr<ID3D12Fence>         Fence;
        wrl::ComPtr<ID3D12InfoQueue>     Info;
        u64  FenceValue = 0;
        UINT Step       = 0;

        wrl::ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE _Tipo, u64 _Bytes, bool _ComUAV,
                                           D3D12_RESOURCE_STATES _Estado) {
            D3D12_HEAP_PROPERTIES HP{};
            HP.Type = _Tipo;
            D3D12_RESOURCE_DESC RD{};
            RD.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            RD.Width            = _Bytes;
            RD.Height           = 1;
            RD.DepthOrArraySize = 1;
            RD.MipLevels        = 1;
            RD.SampleDesc.Count = 1;
            RD.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (_ComUAV) RD.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            wrl::ComPtr<ID3D12Resource> R;
            Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD, _Estado, nullptr,
                                               IID_PPV_ARGS(&R)), "CreateCommittedResource");
            return R;
        }

        wrl::ComPtr<ID3D12Resource> Upload(const void* _Dados, u64 _Bytes) {
            auto R = Buffer(D3D12_HEAP_TYPE_UPLOAD, _Bytes, false,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
            if (_Dados) {
                void* P = nullptr;
                Hr(R->Map(0, nullptr, &P), "Map");
                std::memcpy(P, _Dados, static_cast<size_t>(_Bytes));
                R->Unmap(0, nullptr);
            }
            return R;
        }

        // ERROR e CORRUPTION REPROVAM, e nao apenas aparecem no log. Drenada sempre DEPOIS da
        // fence: antes dela a validacao da execucao ainda nao aconteceu.
        void Drenar(const char* _Quando) {
            if (!Info) return;
            const UINT64 N = Info->GetNumStoredMessages();
            for (UINT64 I = 0; I < N; ++I) {
                SIZE_T Bytes = 0;
                if (FAILED(Info->GetMessage(I, nullptr, &Bytes))) continue;
                std::vector<u8> Buf(Bytes);
                auto* Msg = reinterpret_cast<D3D12_MESSAGE*>(Buf.data());
                if (FAILED(Info->GetMessage(I, Msg, &Bytes))) continue;
                if (Msg->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
                std::cerr << "  [D3D12/" << _Quando << "] " << Msg->pDescription << "\n";
                if (Msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    ++Failures;
                    std::cerr << "  FAIL: a camada de debug reprovou (severidade "
                              << static_cast<int>(Msg->Severity) << ")\n";
                }
            }
            Info->ClearStoredMessages();
        }

        // Fecha, submete, espera a fence e drena a camada de debug.
        void Submeter(ID3D12GraphicsCommandList* _L, const char* _Quando) {
            Hr(_L->Close(), "Close");
            ID3D12CommandList* Listas[] = { _L };
            Queue->ExecuteCommandLists(1, Listas);
            ++FenceValue;
            Hr(Queue->Signal(Fence.Get(), FenceValue), "Signal");
            if (Fence->GetCompletedValue() < FenceValue) {
                struct FEvent {
                    HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    ~FEvent() { if (Value) CloseHandle(Value); }
                } Evento;
                if (!Evento.Value) Hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
                Hr(Fence->SetEventOnCompletion(FenceValue, Evento.Value), "SetEventOnCompletion");
                if (WaitForSingleObject(Evento.Value, 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error(std::string("a execucao nao terminou: ") + _Quando);
            }
            Drenar(_Quando);
        }

        // Uma root signature por passo, com EXATAMENTE os descritores que ele le: CBV, uma tabela de
        // SRVs e uma de UAVs. Reaproveitar uma maior deixaria SRVs do range ligados sem descritor
        // inicializado — coisa que a camada de debug reclama e que nao tem nada a ver com o que esta
        // sob teste.
        void CriarPasso(UINT _Srvs, UINT _Uavs, const std::vector<u8>& _Cs,
                        wrl::ComPtr<ID3D12RootSignature>& _Raiz,
                        wrl::ComPtr<ID3D12PipelineState>& _Pso, const char* _Nome) {
            D3D12_DESCRIPTOR_RANGE Ranges[2]{};
            Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            Ranges[0].NumDescriptors = _Srvs;
            Ranges[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            Ranges[1].NumDescriptors = _Uavs;
            D3D12_ROOT_PARAMETER Params[3]{};
            Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            for (int I = 0; I < 2; ++I) {
                Params[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                Params[I + 1].DescriptorTable.NumDescriptorRanges = 1;
                Params[I + 1].DescriptorTable.pDescriptorRanges   = &Ranges[I];
            }
            D3D12_ROOT_SIGNATURE_DESC Desc{};
            Desc.NumParameters = 3;
            Desc.pParameters   = Params;
            wrl::ComPtr<ID3DBlob> Serial, Erro;
            const std::string Onde = std::string(" (") + _Nome + ")";
            Hr(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Serial, &Erro),
               ("SerializeRootSignature" + Onde).c_str());
            Hr(Device->CreateRootSignature(0, Serial->GetBufferPointer(), Serial->GetBufferSize(),
                                           IID_PPV_ARGS(&_Raiz)),
               ("CreateRootSignature" + Onde).c_str());
            D3D12_COMPUTE_PIPELINE_STATE_DESC PSO{};
            PSO.pRootSignature = _Raiz.Get();
            PSO.CS             = { _Cs.data(), _Cs.size() };
            Hr(Device->CreateComputePipelineState(&PSO, IID_PPV_ARGS(&_Pso)),
               ("CreateComputePipelineState" + Onde).c_str());
        }

        void Construir(const std::vector<u8>& _Classify, const std::vector<u8>& _Scatter,
                       const std::vector<u8>& _Finalize) {
            wrl::ComPtr<ID3D12Debug> Debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug)))) Debug->EnableDebugLayer();

            wrl::ComPtr<IDXGIFactory4> Factory;
            Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1");
            wrl::ComPtr<IDXGIAdapter> Warp;
            Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter");
            Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)),
               "D3D12CreateDevice");
            Device.As(&Info);
            Check(Info != nullptr, "a camada de debug esta ativa");

            // classify: slots + tres tabelas; contagens + contadores.
            CriarPasso(4, 2, _Classify, Root, Pipeline, "classify");
            // scatter: slots + tres tabelas + regioes + contagens; argumentos, cursores, contadores.
            CriarPasso(6, 3, _Scatter, RootScatter, PipelineScatter, "scatter");
            // finalizacao: contagens, regioes, contadores do scatter, cursores, contadores do
            // classify; count buffer + status.
            CriarPasso(5, 2, _Finalize, RootFinalize, PipelineFinalize, "finalizacao");

            D3D12_COMMAND_QUEUE_DESC QD{};
            Hr(Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&Queue)), "CreateCommandQueue");
            Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence");
            Step = Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            Drenar("criacao");
        }

        struct FScatter {
            std::vector<FGpuIndirectDrawArgs> Args;
            std::vector<u32>                  Contadores;
        };

        // O scatter ISOLADO, com regioes e contagens fornecidas pelo chamador.
        //
        // Isolado porque so assim da para entregar ao shader uma tabela que a CPU nao aprovaria — curta
        // demais para o frame, fora do recurso — e ver a DEFESA do scatter agir sozinha, sem a
        // finalizacao depois dela. `IndirectArgs` nasce com o comando POISON em toda posicao: uma
        // posicao que o scatter deixou de escrever aparece como poison, e uma escrita fora da regiao
        // suja uma posicao que devia continuar poison.
        //
        // `_ArgsCapacity` e ao mesmo tempo o tamanho do recurso e o numero que o shader recebe.
        FScatter ExecutarScatter(const FGpuClassifyInput& _In,
                                 std::span<const FGpuIndirectRegion> _Regioes,
                                 std::span<const u32> _Counts, std::span<const u32> _Cursores,
                                 u32 _ArgsCapacity) {
            auto Pisa = [](u64 _Bytes, u64 _Stride) { return std::max<u64>(_Stride, _Bytes); };
            auto BufSlots = Upload(_In.Slots.data(), Pisa(_In.Slots.size() * 4u, 4u));
            auto BufMeta  = Upload(_In.Meta.data(),
                                   Pisa(_In.Meta.size() * sizeof(FGpuSceneInstanceMeta),
                                        sizeof(FGpuSceneInstanceMeta)));
            auto BufMats  = Upload(_In.Materials.data(),
                                   Pisa(_In.Materials.size() * sizeof(FGpuMaterialRecord),
                                        sizeof(FGpuMaterialRecord)));
            auto BufMesh  = Upload(_In.Meshes.data(),
                                   Pisa(_In.Meshes.size() * sizeof(FGpuMeshRecord),
                                        sizeof(FGpuMeshRecord)));
            auto BufReg   = Upload(_Regioes.data(),
                                   Pisa(_Regioes.size() * sizeof(FGpuIndirectRegion),
                                        sizeof(FGpuIndirectRegion)));
            auto BufCounts = Upload(_Counts.data(), Pisa(_Counts.size() * 4u, 4u));

            // Pela porta de dimensionamento, e nunca `_ArgsCapacity * 28` em u32.
            const u64 ArgsBytes = GpuScatterArgsBytes(_ArgsCapacity);
            const u64 ContBytes = GpuScatterCounterCount * sizeof(u32);
            const u64 CurBytes  = Pisa(_Counts.size() * 4u, 4u);
            auto BufArgs = Buffer(D3D12_HEAP_TYPE_DEFAULT, ArgsBytes, true,
                                  D3D12_RESOURCE_STATE_COMMON);
            auto BufCur  = Buffer(D3D12_HEAP_TYPE_DEFAULT, CurBytes, true,
                                  D3D12_RESOURCE_STATE_COMMON);
            auto BufCont = Buffer(D3D12_HEAP_TYPE_DEFAULT, ContBytes, true,
                                  D3D12_RESOURCE_STATE_COMMON);

            const u32 NCmd = static_cast<u32>(ArgsBytes / sizeof(FGpuIndirectDrawArgs));
            const std::vector<FGpuIndirectDrawArgs> Semente(NCmd, ComandoPoison());
            auto BufSemArgs = Upload(Semente.data(), ArgsBytes);
            std::vector<u32> CursoresSeed(static_cast<size_t>(CurBytes / 4), 0u);
            for (size_t K = 0; K < _Cursores.size() && K < CursoresSeed.size(); ++K)
                CursoresSeed[K] = _Cursores[K];
            const u32 ZerosCont[GpuScatterCounterCount]{};
            // Os cursores INICIAIS vem do chamador, e nao de zeros: e assim que se reproduz o cursor
            // sujo de uma execucao anterior sem reset, que a falha de cursor existe para pegar.
            auto BufSemCur = Upload(CursoresSeed.data(), CurBytes);
            auto BufZerosCont = Upload(ZerosCont, ContBytes);

            auto LeituraArgs = Buffer(D3D12_HEAP_TYPE_READBACK, ArgsBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraCont = Buffer(D3D12_HEAP_TYPE_READBACK, ContBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);

            wrl::ComPtr<ID3D12DescriptorHeap> Heap;
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = 9;
            HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Hr(Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");
            auto CpuAt = [&](UINT I) {
                auto H = Heap->GetCPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<SIZE_T>(I) * Step;
                return H;
            };
            auto SRV = [&](ID3D12Resource* R, u64 N, u32 Stride, UINT Slot) {
                D3D12_SHADER_RESOURCE_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max<u32>(1u, static_cast<u32>(N));
                D.Buffer.StructureByteStride = Stride;
                Device->CreateShaderResourceView(R, &D, CpuAt(Slot));
            };
            auto UAV = [&](ID3D12Resource* R, u32 N, u32 Stride, UINT Slot) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max(1u, N);
                D.Buffer.StructureByteStride = Stride;
                Device->CreateUnorderedAccessView(R, nullptr, &D, CpuAt(Slot));
            };
            SRV(BufSlots.Get(), _In.Slots.size(), sizeof(u32), 0);
            SRV(BufMeta.Get(), _In.Meta.size(), sizeof(FGpuSceneInstanceMeta), 1);
            SRV(BufMats.Get(), _In.Materials.size(), sizeof(FGpuMaterialRecord), 2);
            SRV(BufMesh.Get(), _In.Meshes.size(), sizeof(FGpuMeshRecord), 3);
            SRV(BufReg.Get(), _Regioes.size(), sizeof(FGpuIndirectRegion), 4);
            SRV(BufCounts.Get(), _Counts.size(), sizeof(u32), 5);
            UAV(BufArgs.Get(), NCmd, sizeof(FGpuIndirectDrawArgs), 6);
            UAV(BufCur.Get(), static_cast<u32>(CurBytes / 4), sizeof(u32), 7);
            UAV(BufCont.Get(), GpuScatterCounterCount, sizeof(u32), 8);

            alignas(16) u8 CB[256]{};
            const u32 P8[8]{ static_cast<u32>(_In.Slots.size()), static_cast<u32>(_In.Meta.size()),
                             static_cast<u32>(_In.Materials.size()),
                             static_cast<u32>(_In.Meshes.size()), _In.PoolCount, _ArgsCapacity,
                             0u, 0u };
            std::memcpy(CB, P8, sizeof(P8));
            auto BufCB = Upload(CB, sizeof(CB));

            wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
            wrl::ComPtr<ID3D12GraphicsCommandList> List;
            Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&Allocator)), "CreateCommandAllocator");
            Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                         PipelineScatter.Get(), IID_PPV_ARGS(&List)),
               "CreateCommandList (scatter)");
            auto Transicao = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES A,
                                 D3D12_RESOURCE_STATES B) {
                D3D12_RESOURCE_BARRIER Bar{};
                Bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                Bar.Transition.pResource   = R;
                Bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                Bar.Transition.StateBefore = A;
                Bar.Transition.StateAfter  = B;
                List->ResourceBarrier(1, &Bar);
            };
            List->CopyBufferRegion(BufArgs.Get(), 0, BufSemArgs.Get(), 0, ArgsBytes);
            List->CopyBufferRegion(BufCur.Get(), 0, BufSemCur.Get(), 0, CurBytes);
            List->CopyBufferRegion(BufCont.Get(), 0, BufZerosCont.Get(), 0, ContBytes);
            for (ID3D12Resource* R : { BufArgs.Get(), BufCur.Get(), BufCont.Get() })
                Transicao(R, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ID3D12DescriptorHeap* Heaps[] = { Heap.Get() };
            List->SetDescriptorHeaps(1, Heaps);
            List->SetComputeRootSignature(RootScatter.Get());
            List->SetComputeRootConstantBufferView(0, BufCB->GetGPUVirtualAddress());
            auto GpuAt = [&](UINT I) {
                auto H = Heap->GetGPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<UINT64>(I) * Step;
                return H;
            };
            List->SetComputeRootDescriptorTable(1, GpuAt(0));
            List->SetComputeRootDescriptorTable(2, GpuAt(6));
            const u32 Total = static_cast<u32>(_In.Slots.size());
            if (Total > 0) List->Dispatch((Total + 63u) / 64u, 1, 1);

            Transicao(BufArgs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraArgs.Get(), BufArgs.Get());
            List->CopyResource(LeituraCont.Get(), BufCont.Get());
            Submeter(List.Get(), "scatter isolado");

            FScatter Saida;
            Saida.Args.resize(NCmd);
            Saida.Contadores.resize(GpuScatterCounterCount);
            {
                const u8* P = nullptr;
                Hr(LeituraArgs->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u8**>(&P))),
                   "Map args");
                std::memcpy(Saida.Args.data(), P, static_cast<size_t>(ArgsBytes));
                LeituraArgs->Unmap(0, nullptr);
            }
            {
                const u32* P = nullptr;
                Hr(LeituraCont->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u32**>(&P))),
                   "Map cont scatter");
                std::memcpy(Saida.Contadores.data(), P, static_cast<size_t>(ContBytes));
                LeituraCont->Unmap(0, nullptr);
            }
            return Saida;
        }

        // O classify ISOLADO: contagens e contadores de desfecho.
        FResultado Executar(const FGpuClassifyInput& _In) {
            // Piso de UM ELEMENTO, e nao de um byte: um SRV de um elemento sobre um recurso de 1 B
            // e ilegal e desfaz o device no dispatch seguinte, longe da causa.
            auto Pisa = [](u64 _Bytes, u64 _Stride) { return std::max<u64>(_Stride, _Bytes); };
            auto BufSlots = Upload(_In.Slots.data(),
                                   Pisa(_In.Slots.size() * sizeof(u32), sizeof(u32)));
            auto BufMeta  = Upload(_In.Meta.data(),
                                   Pisa(_In.Meta.size() * sizeof(FGpuSceneInstanceMeta),
                                        sizeof(FGpuSceneInstanceMeta)));
            auto BufMats  = Upload(_In.Materials.data(),
                                   Pisa(_In.Materials.size() * sizeof(FGpuMaterialRecord),
                                        sizeof(FGpuMaterialRecord)));
            auto BufMesh  = Upload(_In.Meshes.data(),
                                   Pisa(_In.Meshes.size() * sizeof(FGpuMeshRecord),
                                        sizeof(FGpuMeshRecord)));

            const u64 LoteBytes = Pisa(_In.BatchCapacity * sizeof(u32), sizeof(u32));
            const u64 ContBytes = GpuClassifyOutcomeCount * sizeof(u32);
            // COMMON, e nao COPY_DEST: o D3D12 IGNORA o estado inicial de um buffer (a camada de
            // debug avisa), e todo buffer nasce em COMMON. A copia de zeros logo abaixo promove
            // implicitamente para COPY_DEST, que e de onde a barreira seguinte parte.
            auto BufLotes = Buffer(D3D12_HEAP_TYPE_DEFAULT, LoteBytes, true,
                                   D3D12_RESOURCE_STATE_COMMON);
            auto BufCont  = Buffer(D3D12_HEAP_TYPE_DEFAULT, ContBytes, true,
                                   D3D12_RESOURCE_STATE_COMMON);
            // Os zeros vem de uma COPIA: um DEFAULT recem-criado nao promete conteudo, e um
            // histograma somado sobre lixo daria numeros plausiveis e falsos.
            const std::vector<u32> ZerosLote(static_cast<size_t>(LoteBytes / 4), 0u);
            const u32 ZerosCont[GpuClassifyOutcomeCount]{};
            auto BufZerosLote = Upload(ZerosLote.data(), LoteBytes);
            auto BufZerosCont = Upload(ZerosCont, ContBytes);

            auto LeituraLotes = Buffer(D3D12_HEAP_TYPE_READBACK, LoteBytes, false,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraCont  = Buffer(D3D12_HEAP_TYPE_READBACK, ContBytes, false,
                                       D3D12_RESOURCE_STATE_COPY_DEST);

            wrl::ComPtr<ID3D12DescriptorHeap> Heap;
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = 6;
            HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Hr(Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");

            auto CpuAt = [&](UINT I) {
                auto H = Heap->GetCPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<SIZE_T>(I) * Step;
                return H;
            };
            auto SRV = [&](ID3D12Resource* R, u64 Elementos, u32 Stride, UINT Slot) {
                D3D12_SHADER_RESOURCE_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max<u32>(1u, static_cast<u32>(Elementos));
                D.Buffer.StructureByteStride = Stride;
                Device->CreateShaderResourceView(R, &D, CpuAt(Slot));
            };
            auto UAV = [&](ID3D12Resource* R, u32 Elementos, UINT Slot) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max(1u, Elementos);
                D.Buffer.StructureByteStride = sizeof(u32);
                Device->CreateUnorderedAccessView(R, nullptr, &D, CpuAt(Slot));
            };
            SRV(BufSlots.Get(), _In.Slots.size(), sizeof(u32), 0);
            SRV(BufMeta.Get(), _In.Meta.size(), sizeof(FGpuSceneInstanceMeta), 1);
            SRV(BufMats.Get(), _In.Materials.size(), sizeof(FGpuMaterialRecord), 2);
            SRV(BufMesh.Get(), _In.Meshes.size(), sizeof(FGpuMeshRecord), 3);
            UAV(BufLotes.Get(), _In.BatchCapacity, 4);
            UAV(BufCont.Get(), GpuClassifyOutcomeCount, 5);

            alignas(16) u8 CB[256]{};
            const u32 P8[8]{ static_cast<u32>(_In.Slots.size()),
                             static_cast<u32>(_In.Meta.size()),
                             static_cast<u32>(_In.Materials.size()),
                             static_cast<u32>(_In.Meshes.size()),
                             _In.PoolCount, _In.BatchCapacity, 0u, 0u };
            std::memcpy(CB, P8, sizeof(P8));
            auto BufCB = Upload(CB, sizeof(CB));

            wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
            wrl::ComPtr<ID3D12GraphicsCommandList> List;
            Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&Allocator)), "CreateCommandAllocator");
            Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                         Pipeline.Get(), IID_PPV_ARGS(&List)), "CreateCommandList");

            auto Transicao = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES A,
                                 D3D12_RESOURCE_STATES B) {
                D3D12_RESOURCE_BARRIER Bar{};
                Bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                Bar.Transition.pResource   = R;
                Bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                Bar.Transition.StateBefore = A;
                Bar.Transition.StateAfter  = B;
                List->ResourceBarrier(1, &Bar);
            };
            List->CopyBufferRegion(BufLotes.Get(), 0, BufZerosLote.Get(), 0, LoteBytes);
            List->CopyBufferRegion(BufCont.Get(), 0, BufZerosCont.Get(), 0, ContBytes);
            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ID3D12DescriptorHeap* Heaps[] = { Heap.Get() };
            List->SetDescriptorHeaps(1, Heaps);
            List->SetComputeRootSignature(Root.Get());
            List->SetComputeRootConstantBufferView(0, BufCB->GetGPUVirtualAddress());
            auto GpuAt = [&](UINT I) {
                auto H = Heap->GetGPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<UINT64>(I) * Step;
                return H;
            };
            List->SetComputeRootDescriptorTable(1, GpuAt(0));
            List->SetComputeRootDescriptorTable(2, GpuAt(4));
            const u32 Total = static_cast<u32>(_In.Slots.size());
            if (Total > 0) List->Dispatch((Total + 63u) / 64u, 1, 1);

            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraLotes.Get(), BufLotes.Get());
            List->CopyResource(LeituraCont.Get(), BufCont.Get());
            Submeter(List.Get(), "classify isolado");

            FResultado Saida;
            Saida.Lotes.resize(_In.BatchCapacity);
            Saida.Contadores.resize(GpuClassifyOutcomeCount);
            if (_In.BatchCapacity > 0) {
                const u32* P = nullptr;
                Hr(LeituraLotes->Map(0, nullptr,
                                     reinterpret_cast<void**>(const_cast<u32**>(&P))), "Map lotes");
                std::memcpy(Saida.Lotes.data(), P, _In.BatchCapacity * sizeof(u32));
                LeituraLotes->Unmap(0, nullptr);
            }
            {
                const u32* P = nullptr;
                Hr(LeituraCont->Map(0, nullptr,
                                    reinterpret_cast<void**>(const_cast<u32**>(&P))), "Map cont");
                std::memcpy(Saida.Contadores.data(), P, ContBytes);
                LeituraCont->Unmap(0, nullptr);
            }
            return Saida;
        }
    };
}

namespace {
    // A CADEIA inteira, sobre recursos PERSISTENTES:
    //
    //     reset -> classify -> scatter -> finalizacao
    //
    // Os executores de cima criam buffers novos a cada chamada, e por isso nao enxergam o defeito que
    // esta estrutura existe para pegar: um acumulador que atravessa de uma execucao para a seguinte.
    // Aqui os recursos sao criados UMA vez e `Rodar` pode ser chamado de novo sobre eles, como o
    // frame faria.
    //
    // O estado de cada recurso e rastreado por uma tabela, e toda barreira parte do estado real.
    // Com a camada de debug reprovando, um `StateBefore` errado entre execucoes vira falha de teste
    // em vez de passar despercebido.
    struct FCadeia {
        FSonda&                  S;
        const FGpuClassifyInput& In;
        u32                      Slots      = 0; // slots de lote
        u32                      Capacidade = 0; // comandos do RECURSO IndirectArgs

        // A tabela de regioes e a capacidade que o SHADER recebe. Membros, e nao constantes da criacao:
        // sobem a cada execucao, como o frame faria, e os cenarios de falha os trocam para entregar a
        // GPU uma tabela que a CPU nao aprovou.
        std::vector<FGpuIndirectRegion> Regioes;
        u32                             CapacidadeShader = 0;

        wrl::ComPtr<ID3D12Resource> BufSlots, BufMeta, BufMats, BufMesh, BufZeros;
        wrl::ComPtr<ID3D12Resource> Counts, ClassCont;
        wrl::ComPtr<ID3D12Resource> Args, Cursors, ScatCont, Draw, Status;
        wrl::ComPtr<ID3D12DescriptorHeap> Heap;
        std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> Estados;

        struct FSaida {
            std::vector<u32>                  Counts, Cursors, ScatCont, Draw, ClassCont;
            std::vector<FGpuIndirectDrawArgs> Args;
            u32                               Status = 0;
            u32                               Reason = 0;
        };

        FCadeia(FSonda& _S, const FGpuClassifyInput& _In,
                std::span<const FGpuIndirectRegion> _Regioes, u32 _Capacidade)
            : S(_S), In(_In), Slots(_In.BatchCapacity), Capacidade(_Capacidade),
              Regioes(_Regioes.begin(), _Regioes.end()), CapacidadeShader(_Capacidade) {}

        D3D12_RESOURCE_STATES& EstadoDe(ID3D12Resource* _R) {
            for (auto& E : Estados) if (E.first == _R) return E.second;
            Estados.push_back({ _R, D3D12_RESOURCE_STATE_COMMON });
            return Estados.back().second;
        }

        void Criar() {
            auto Pisa = [](u64 _B, u64 _S) { return std::max<u64>(_S, _B); };
            BufSlots = S.Upload(In.Slots.data(), Pisa(In.Slots.size() * 4u, 4u));
            BufMeta  = S.Upload(In.Meta.data(), Pisa(In.Meta.size() * sizeof(FGpuSceneInstanceMeta),
                                                     sizeof(FGpuSceneInstanceMeta)));
            BufMats  = S.Upload(In.Materials.data(),
                                Pisa(In.Materials.size() * sizeof(FGpuMaterialRecord),
                                     sizeof(FGpuMaterialRecord)));
            BufMesh  = S.Upload(In.Meshes.data(), Pisa(In.Meshes.size() * sizeof(FGpuMeshRecord),
                                                       sizeof(FGpuMeshRecord)));

            const u64 LoteB = Pisa(Slots * 4u, 4u);
            auto Uav = [&](u64 _Bytes) {
                return S.Buffer(D3D12_HEAP_TYPE_DEFAULT, _Bytes, true, D3D12_RESOURCE_STATE_COMMON);
            };
            Counts    = Uav(LoteB);
            ClassCont = Uav(GpuClassifyOutcomeCount * 4u);
            Args      = Uav(GpuScatterArgsBytes(Capacidade));
            Cursors   = Uav(LoteB);
            ScatCont  = Uav(GpuScatterCounterCount * 4u);
            Draw      = Uav(LoteB);
            Status    = Uav(8u);

            // Zeros do tamanho do maior acumulador. O reset copia daqui.
            const u64 MaiorB = std::max<u64>(LoteB, GpuClassifyOutcomeCount * 4u);
            const std::vector<u8> Z(static_cast<size_t>(MaiorB), 0u);
            BufZeros = S.Upload(Z.data(), MaiorB);

            // `IndirectArgs` recebe o POISON uma unica vez, na criacao. Nas execucoes seguintes ele NAO
            // e ressemeado: se uma execucao deixasse de escrever uma posicao, o que apareceria ali seria
            // o comando da execucao anterior — exatamente o "comando velho" que a finalizacao existe
            // para nunca deixar ser desenhado.
            const u32 NCmd = static_cast<u32>(GpuScatterArgsBytes(Capacidade) / 28u);
            const std::vector<FGpuIndirectDrawArgs> Sem(NCmd, ComandoPoison());
            auto BufSem = S.Upload(Sem.data(), GpuScatterArgsBytes(Capacidade));
            Executar([&](ID3D12GraphicsCommandList* L) {
                Ir(L, Args.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                L->CopyBufferRegion(Args.Get(), 0, BufSem.Get(), 0, GpuScatterArgsBytes(Capacidade));
            }, "semente");

            CriarDescritores();
        }

        // Transicao EXPLICITA a partir do estado rastreado. Nada aqui depende de promocao implicita:
        // estado implicito decai no fim da command list, e um rastreador que o seguisse erraria o
        // `StateBefore` da execucao seguinte.
        void Ir(ID3D12GraphicsCommandList* _L, ID3D12Resource* _R, D3D12_RESOURCE_STATES _Novo) {
            D3D12_RESOURCE_STATES& Atual = EstadoDe(_R);
            if (Atual == _Novo) return;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = _R;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = Atual;
            B.Transition.StateAfter  = _Novo;
            _L->ResourceBarrier(1, &B);
            Atual = _Novo;
        }

        // Grava e submete. Uma command list por chamada.
        template <typename TGravar>
        void Executar(TGravar&& _Gravar, const char* _Quando) {
            wrl::ComPtr<ID3D12CommandAllocator>    Alloc;
            wrl::ComPtr<ID3D12GraphicsCommandList> L;
            Hr(S.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&Alloc)), "CreateCommandAllocator");
            Hr(S.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&L)), "CreateCommandList (cadeia)");
            _Gravar(L.Get());
            S.Submeter(L.Get(), _Quando);
        }

        // Tabelas de descritores, uma por passo, na ordem das root signatures de cada um.
        //   classify: SRV 0-3   (slots, meta, mats, mesh)                        UAV 4-5   (counts, classcont)
        //   scatter:  SRV 6-11  (slots, meta, mats, mesh, regioes, counts)       UAV 12-14 (args, cursors, scatcont)
        //   final:    SRV 15-19 (counts, regioes, scatcont, cursors, classcont)  UAV 20-21 (draw, status)
        //
        // As duas SRVs de REGIOES (10 e 16) sao escritas a cada execucao, junto do upload da tabela.
        static constexpr UINT kClassSrv = 0, kClassUav = 4, kScatSrv = 6, kScatUav = 12;
        static constexpr UINT kFinSrv = 15, kFinUav = 20, kDescritores = 22;

        D3D12_CPU_DESCRIPTOR_HANDLE CpuAt(UINT _I) const {
            auto H = Heap->GetCPUDescriptorHandleForHeapStart();
            H.ptr += static_cast<SIZE_T>(_I) * S.Step;
            return H;
        }
        void Srv(ID3D12Resource* _R, u64 _N, u32 _Stride, UINT _I) {
            D3D12_SHADER_RESOURCE_VIEW_DESC D{};
            D.Format                     = DXGI_FORMAT_UNKNOWN;
            D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            D.Buffer.NumElements         = std::max<u32>(1u, static_cast<u32>(_N));
            D.Buffer.StructureByteStride = _Stride;
            S.Device->CreateShaderResourceView(_R, &D, CpuAt(_I));
        }
        void Uav(ID3D12Resource* _R, u64 _N, u32 _Stride, UINT _I) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC D{};
            D.Format                     = DXGI_FORMAT_UNKNOWN;
            D.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
            D.Buffer.NumElements         = std::max<u32>(1u, static_cast<u32>(_N));
            D.Buffer.StructureByteStride = _Stride;
            S.Device->CreateUnorderedAccessView(_R, nullptr, &D, CpuAt(_I));
        }

        void CriarDescritores() {
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = kDescritores;
            HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Hr(S.Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");
            const u32 NCmd = static_cast<u32>(GpuScatterArgsBytes(Capacidade) / 28u);
            const u32 MetaStride = sizeof(FGpuSceneInstanceMeta);
            const u32 MatStride  = sizeof(FGpuMaterialRecord);
            const u32 MeshStride = sizeof(FGpuMeshRecord);
            // classify
            Srv(BufSlots.Get(), In.Slots.size(), 4u, kClassSrv + 0);
            Srv(BufMeta.Get(), In.Meta.size(), MetaStride, kClassSrv + 1);
            Srv(BufMats.Get(), In.Materials.size(), MatStride, kClassSrv + 2);
            Srv(BufMesh.Get(), In.Meshes.size(), MeshStride, kClassSrv + 3);
            Uav(Counts.Get(), Slots, 4u, kClassUav + 0);
            Uav(ClassCont.Get(), GpuClassifyOutcomeCount, 4u, kClassUav + 1);
            // scatter (a SRV 4, das regioes, e escrita por execucao)
            Srv(BufSlots.Get(), In.Slots.size(), 4u, kScatSrv + 0);
            Srv(BufMeta.Get(), In.Meta.size(), MetaStride, kScatSrv + 1);
            Srv(BufMats.Get(), In.Materials.size(), MatStride, kScatSrv + 2);
            Srv(BufMesh.Get(), In.Meshes.size(), MeshStride, kScatSrv + 3);
            Srv(Counts.Get(), Slots, 4u, kScatSrv + 5);
            Uav(Args.Get(), NCmd, sizeof(FGpuIndirectDrawArgs), kScatUav + 0);
            Uav(Cursors.Get(), Slots, 4u, kScatUav + 1);
            Uav(ScatCont.Get(), GpuScatterCounterCount, 4u, kScatUav + 2);
            // finalizacao (a SRV 1, das regioes, e escrita por execucao)
            Srv(Counts.Get(), Slots, 4u, kFinSrv + 0);
            Srv(ScatCont.Get(), GpuScatterCounterCount, 4u, kFinSrv + 2);
            Srv(Cursors.Get(), Slots, 4u, kFinSrv + 3);
            Srv(ClassCont.Get(), GpuClassifyOutcomeCount, 4u, kFinSrv + 4);
            Uav(Draw.Get(), Slots, 4u, kFinUav + 0);
            Uav(Status.Get(), 2u, 4u, kFinUav + 1);
        }

        // Quais acumuladores o reset zera. `kResetTudo` e o frame normal; as mascaras parciais existem
        // para o teste provar cada elo da finalizacao pela SUA causa — um reset esquecido de cada vez,
        // com o motivo exato que ele tem de produzir.
        static constexpr u32 kResetCounts = 1u, kResetClassCont = 2u, kResetCursors = 4u,
                             kResetScatCont = 8u, kResetTudo = 15u;

        // Uma execucao da cadeia.
        FSaida Rodar(u32 _Reset) {
            const u32 Total = static_cast<u32>(In.Slots.size());
            const u32 NCmd  = static_cast<u32>(GpuScatterArgsBytes(Capacidade) / 28u);
            auto Cb = [&](std::initializer_list<u32> _V) {
                alignas(16) u8 B[256]{};
                u32 K = 0;
                for (u32 X : _V) { std::memcpy(B + 4u * K, &X, 4u); ++K; }
                return S.Upload(B, sizeof(B));
            };
            auto CbClass = Cb({ Total, static_cast<u32>(In.Meta.size()),
                                static_cast<u32>(In.Materials.size()),
                                static_cast<u32>(In.Meshes.size()), In.PoolCount, Slots });
            auto CbScat  = Cb({ Total, static_cast<u32>(In.Meta.size()),
                                static_cast<u32>(In.Materials.size()),
                                static_cast<u32>(In.Meshes.size()), In.PoolCount, CapacidadeShader });
            auto CbFin   = Cb({ Slots, Total });

            // A tabela de regioes DESTA execucao, e as duas views que a enxergam. Sobe a cada vez, como
            // o frame faria com a tabela do seu frame slot.
            auto BufReg = S.Upload(Regioes.data(),
                                   std::max<u64>(sizeof(FGpuIndirectRegion),
                                                 Regioes.size() * sizeof(FGpuIndirectRegion)));
            Srv(BufReg.Get(), Regioes.size(), sizeof(FGpuIndirectRegion), kScatSrv + 4);
            Srv(BufReg.Get(), Regioes.size(), sizeof(FGpuIndirectRegion), kFinSrv + 1);

            const u64 LoteB = std::max<u64>(4u, Slots * 4u);
            auto Leitura = [&](u64 _B) {
                return S.Buffer(D3D12_HEAP_TYPE_READBACK, _B, false, D3D12_RESOURCE_STATE_COPY_DEST);
            };
            auto LCounts = Leitura(LoteB), LCursors = Leitura(LoteB), LDraw = Leitura(LoteB);
            auto LScat   = Leitura(GpuScatterCounterCount * 4u);
            auto LStatus = Leitura(8u);
            auto LClass  = Leitura(GpuClassifyOutcomeCount * 4u);
            auto LArgs   = Leitura(GpuScatterArgsBytes(Capacidade));

            auto GpuAt = [&](UINT I) {
                auto H = Heap->GetGPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<UINT64>(I) * S.Step;
                return H;
            };

            Executar([&](ID3D12GraphicsCommandList* L) {
                const auto SRV_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                const auto UAV_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                ID3D12DescriptorHeap* Heaps[] = { Heap.Get() };
                L->SetDescriptorHeaps(1, Heaps);

                // 0. RESET — os acumuladores que a mascara pede. Com `kResetTudo`, os quatro.
                const std::pair<u32, ID3D12Resource*> Acumuladores[] = {
                    { kResetCounts, Counts.Get() },     { kResetClassCont, ClassCont.Get() },
                    { kResetCursors, Cursors.Get() },   { kResetScatCont, ScatCont.Get() },
                };
                for (const auto& A : Acumuladores) {
                    if ((_Reset & A.first) == 0u) continue;
                    Ir(L, A.second, D3D12_RESOURCE_STATE_COPY_DEST);
                    L->CopyBufferRegion(A.second, 0, BufZeros.Get(), 0, A.second->GetDesc().Width);
                }

                // 1. classify
                Ir(L, Counts.Get(), UAV_); Ir(L, ClassCont.Get(), UAV_);
                L->SetPipelineState(S.Pipeline.Get());
                L->SetComputeRootSignature(S.Root.Get());
                L->SetComputeRootConstantBufferView(0, CbClass->GetGPUVirtualAddress());
                L->SetComputeRootDescriptorTable(1, GpuAt(kClassSrv));
                L->SetComputeRootDescriptorTable(2, GpuAt(kClassUav));
                if (Total > 0) L->Dispatch((Total + 63u) / 64u, 1, 1);

                // 2. scatter
                //
                // A transicao UAV -> NON_PIXEL_SHADER_RESOURCE de `Counts` e o contrato da secao 6.4 do
                // plano, e e ela que serve de ponto de sincronizacao entre os dois dispatches.
                //
                // ⚠️ Este teste NAO prova que ela e necessaria, e isso ja foi verificado: removendo-a,
                // tudo continua verde. Dois motivos, os dois do ambiente e nao do codigo — buffers em
                // D3D12 tem promocao e decaimento implicitos de estado, entao a camada de debug nao
                // reprova a leitura por SRV de um buffer "em" UAV; e o WARP completa o primeiro dispatch
                // antes do segundo, o que esconde a corrida. A ausencia da barreira e uma CORRIDA, e
                // corrida nao se prova por teste deterministico. Ela fica porque o contrato exige, nao
                // porque algo aqui a exija.
                Ir(L, Counts.Get(), SRV_);
                Ir(L, Args.Get(), UAV_); Ir(L, Cursors.Get(), UAV_); Ir(L, ScatCont.Get(), UAV_);
                L->SetPipelineState(S.PipelineScatter.Get());
                L->SetComputeRootSignature(S.RootScatter.Get());
                L->SetComputeRootConstantBufferView(0, CbScat->GetGPUVirtualAddress());
                L->SetComputeRootDescriptorTable(1, GpuAt(kScatSrv));
                L->SetComputeRootDescriptorTable(2, GpuAt(kScatUav));
                if (Total > 0) L->Dispatch((Total + 63u) / 64u, 1, 1);

                // 3. finalizacao
                Ir(L, ScatCont.Get(), SRV_); Ir(L, Cursors.Get(), SRV_); Ir(L, ClassCont.Get(), SRV_);
                Ir(L, Draw.Get(), UAV_); Ir(L, Status.Get(), UAV_);
                L->SetPipelineState(S.PipelineFinalize.Get());
                L->SetComputeRootSignature(S.RootFinalize.Get());
                L->SetComputeRootConstantBufferView(0, CbFin->GetGPUVirtualAddress());
                L->SetComputeRootDescriptorTable(1, GpuAt(kFinSrv));
                L->SetComputeRootDescriptorTable(2, GpuAt(kFinUav));
                L->Dispatch(1, 1, 1);

                // 4. leitura
                const auto CS = D3D12_RESOURCE_STATE_COPY_SOURCE;
                for (ID3D12Resource* R : { Counts.Get(), Cursors.Get(), Draw.Get(), ScatCont.Get(),
                                           ClassCont.Get(), Status.Get(), Args.Get() })
                    Ir(L, R, CS);
                L->CopyResource(LCounts.Get(), Counts.Get());
                L->CopyResource(LCursors.Get(), Cursors.Get());
                L->CopyResource(LDraw.Get(), Draw.Get());
                L->CopyResource(LScat.Get(), ScatCont.Get());
                L->CopyResource(LStatus.Get(), Status.Get());
                L->CopyResource(LClass.Get(), ClassCont.Get());
                L->CopyResource(LArgs.Get(), Args.Get());
            }, _Reset == kResetTudo ? "cadeia" : "cadeia com reset parcial");

            auto Ler = [](ID3D12Resource* _R, u64 _Bytes) {
                std::vector<u32> V(static_cast<size_t>(_Bytes / 4), 0u);
                const u32* P = nullptr;
                Hr(_R->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u32**>(&P))), "Map");
                std::memcpy(V.data(), P, static_cast<size_t>(_Bytes));
                _R->Unmap(0, nullptr);
                return V;
            };
            FSaida O;
            O.Counts   = Ler(LCounts.Get(), Slots * 4u);
            O.Cursors  = Ler(LCursors.Get(), Slots * 4u);
            O.Draw     = Ler(LDraw.Get(), Slots * 4u);
            O.ScatCont = Ler(LScat.Get(), GpuScatterCounterCount * 4u);
            const std::vector<u32> St = Ler(LStatus.Get(), 8u);
            O.Status    = St[0];
            O.Reason    = St[1];
            O.ClassCont = Ler(LClass.Get(), GpuClassifyOutcomeCount * 4u);
            {
                O.Args.resize(NCmd);
                const u8* P = nullptr;
                Hr(LArgs->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u8**>(&P))), "Map args");
                std::memcpy(O.Args.data(), P, static_cast<size_t>(GpuScatterArgsBytes(Capacidade)));
                LArgs->Unmap(0, nullptr);
            }
            return O;
        }
    };
}

int main() try {
    std::cout << "Smile.GpuClassifyWarp\n";

    TestarDesfechos();
    TestarIndiceDeLote();
    TestarPortaDeDimensionamento();
    TestarCapacidades();
    TestarLayout();
    TestarPreflightDeRegioes();
    TestarUmResultadoPorCandidato();
    TestarParticoes();
    TestarPreflight();
    TestarComando();
    TestarScatterCpu();

    const std::vector<u8> Bytecode = LerArquivo(SMILE_CLASSIFY_CANDIDATES_CSO);
    Check(!Bytecode.empty(), "a sonda compilada foi encontrada");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_CLASSIFY_CANDIDATES_CSO << ")\n";
        return 1;
    }
    const std::vector<u8> BytecodeScatter = LerArquivo(SMILE_SCATTER_INDIRECT_ARGS_CSO);
    Check(!BytecodeScatter.empty(), "a sonda do scatter foi encontrada");
    if (BytecodeScatter.empty()) {
        std::cerr << "  (esperado em " << SMILE_SCATTER_INDIRECT_ARGS_CSO << ")\n";
        return 1;
    }
    const std::vector<u8> BytecodeFinalize = LerArquivo(SMILE_FINALIZE_DRAW_COUNTS_CSO);
    Check(!BytecodeFinalize.empty(), "a sonda da finalizacao foi encontrada");
    if (BytecodeFinalize.empty()) {
        std::cerr << "  (esperado em " << SMILE_FINALIZE_DRAW_COUNTS_CSO << ")\n";
        return 1;
    }

    FSonda S;
    S.Construir(Bytecode, BytecodeScatter, BytecodeFinalize);

    const auto Mats = Materiais(); const auto Meshs = Malhas();
    const auto Metas = Meta();     const auto Slot = Slots();

    FGpuClassifyInput In{};
    In.Slots = Slot; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
    In.PoolCount = kPoolCount;
    In.BatchCapacity = CapacidadeDeLote(kPoolCount);
    const u32 Candidatos = static_cast<u32>(Slot.size());

    std::vector<u32> LotesCpu, ContCpu;
    ClassifyCandidates(In, LotesCpu, ContCpu);
    const FResultado Gpu = S.Executar(In);

    // O oraculo dos contadores e escrito a mao, e nao tirado da referencia CPU: as duas
    // implementacoes sao espelhos do mesmo raciocinio, e compara-las so entre si deixaria passar um
    // raciocinio errado nos dois lados.
    u32 Querido[GpuClassifyOutcomeCount]{};
    Querido[GpuClassifyIndirect]          = 3u; // slots 0, 1 e 2
    Querido[GpuClassifyNoSlot]            = 1u;
    Querido[GpuClassifyInvalidSlot]       = 2u; // linha vazia + slot fora da tabela
    Querido[GpuClassifyInvalidMaterialId] = 1u;
    Querido[GpuClassifyInvalidMeshId]     = 1u;
    Querido[GpuClassifyInvalidPoolId]     = 1u;
    Querido[GpuClassifyNotEligible]       = 4u; // two-sided, masked, blend, e o de dois termos
    Querido[GpuClassifyMeshUnusable]   = 1u;
    Querido[GpuClassifyOverflow]          = 0u;

    u32 SomaGpu = 0;
    for (u32 O = 0; O < GpuClassifyOutcomeCount; ++O) {
        CheckEq(Gpu.Contadores[O], Querido[O],
                std::string("contador ") + NomeDesfecho(O) + " na GPU");
        CheckEq(ContCpu[O], Querido[O], std::string("contador ") + NomeDesfecho(O) + " na CPU");
        SomaGpu += Gpu.Contadores[O];
    }
    CheckEq(SomaGpu, Candidatos, "na GPU tambem: um desfecho por candidato, nem mais nem menos");

    // Os lotes, elemento a elemento. O lote 0 tem DOIS comandos: sem ele, um shader que
    // sobrescrevesse em vez de somar passaria.
    const u32 QueridoLote[8]{ 2u, 1u, 0u, 0u, 0u, 0u, 0u, 0u };
    for (u32 B = 0; B < In.BatchCapacity; ++B) {
        CheckEq(Gpu.Lotes[B], QueridoLote[B], "lote " + std::to_string(B) + " na GPU");
        CheckEq(LotesCpu[B], QueridoLote[B], "lote " + std::to_string(B) + " na CPU");
    }
    // As faixas das classes 1..3 ficam em ZERO. Nao e redundancia com o laco acima: e o que prova
    // que a elegibilidade BARROU, e nao que o dado dessas classes faltou — a cena tem uma instancia
    // de cada uma delas, e as tres estao contadas em NotEligible.
    for (u32 C = 1; C < kGpuPipelineClassCount; ++C)
        for (u32 P = 0; P < kPoolCount; ++P)
            CheckEq(Gpu.Lotes[GpuBatchIndex({ C, P }, kPoolCount)], 0u,
                    "classe " + std::to_string(C) + " nao emite comando nesta fase");


    // ---- as REGIOES da cena, pelas funcoes de PRODUCAO ---------------------------------------
    //
    // Calculadas das TABELAS, e de nada que o dispatch acima produziu: nem das contagens da GPU, que
    // seria readback, nem das da CPU, que seria reproduzir o frame. Todo o resto deste teste consome
    // estas regioes — o scatter isolado e a cadeia —, e nenhum offset e fornecido pelo fixture.
    std::vector<u64> CapReg;
    std::vector<FGpuIndirectRegion> Regioes;
    u32 TotalReg = 0u;
    CheckEq(GpuCountRegionCapacities(Metas, Mats, Meshs, kPoolCount, CapReg), GpuClassifyPreflightOk,
            "capacidades das regioes da cena");
    CheckEq(GpuLayoutRegions(CapReg, Regioes, TotalReg), GpuRegionLayoutOk,
            "layout das regioes da cena");
    CheckEq(GpuRegionPreflight(Regioes, In.BatchCapacity, TotalReg), GpuRegionPreflightOk,
            "e a porta das regioes aprova, com IndirectArgs do tamanho exato do total");
    // A FOLGA, que e o que distingue offset de regiao de prefixo das contagens: com prefixo, o lote 1
    // comecaria em 2.
    Check(Regioes.size() > 1 && Regioes[1].Offset == 4u && LotesCpu[0] == 2u,
          "o lote 1 comeca em 4, depois da folga do lote 0, e nao na contagem dele");

    // ---- SCATTER em WARP, isolado ---------------------------------------------------------------
    //
    // `IndirectArgs` nasce com o comando POISON em toda posicao. Duas coisas ficam visiveis assim:
    // BURACO (posicao reservada que o scatter nao escreveu: poison entre os comandos de um lote) e
    // escrita FORA DA REGIAO (comando onde devia continuar poison). A comparacao dentro da regiao e
    // por MULTICONJUNTO, porque a ordem la depende de qual thread chega primeiro ao cursor.
    {
        const FSonda::FScatter G = S.ExecutarScatter(In, Regioes, LotesCpu, {}, TotalReg);

        CheckEq(G.Contadores[GpuScatterCounterEmitted], Gpu.Contadores[GpuClassifyIndirect],
                "scatter na GPU emite exatamente o que o classify contou");
        CheckEq(G.Contadores[GpuScatterCounterFaultCursor], 0u, "cursores zerados: sem falha de cursor");
        CheckEq(G.Contadores[GpuScatterCounterFaultRegion], 0u, "regioes das tabelas: sem falha de regiao");
        CheckEq(G.Contadores[GpuScatterCounterFaultCapacity], 0u, "regioes no recurso: sem falha de capacidade");
        ConferirArgsDaCena(G.Args, "GPU");

        // GPU x CPU, regiao a regiao como multiconjunto, e a folga byte a byte. A referencia CPU
        // recebe a MESMA semente, entao o que ficou intocado tambem tem de coincidir.
        std::vector<FGpuIndirectDrawArgs> ArgsCpu(TotalReg, ComandoPoison());
        std::vector<u32> ContScatterCpu;
        std::vector<u32> CursoresCpu(LotesCpu.size(), 0u);
        ScatterIndirectArgs(In, Regioes, LotesCpu, CursoresCpu, ArgsCpu, ContScatterCpu);
        for (u32 K = 0; K < GpuScatterCounterCount; ++K)
            CheckEq(ContScatterCpu[K], G.Contadores[K],
                    "contador " + std::to_string(K) + " do scatter: CPU == GPU");
        for (u32 B = 0; B < In.BatchCapacity; ++B) {
            const FGpuIndirectRegion& R = Regioes[B];
            if (R.Capacity == 0u) continue;
            const auto A  = Ordenados(&G.Args[R.Offset], LotesCpu[B]);
            const auto Cc = Ordenados(&ArgsCpu[R.Offset], LotesCpu[B]);
            bool Iguais = A.size() == Cc.size();
            for (size_t K = 0; Iguais && K < A.size(); ++K) Iguais = MesmoComando(A[K], Cc[K]);
            Check(Iguais, "regiao " + std::to_string(B) + ": multiconjunto GPU == CPU");
            for (u32 P = R.Offset + LotesCpu[B]; P < R.Offset + R.Capacity; ++P)
                Check(MesmoComando(G.Args[P], ArgsCpu[P]),
                      "folga na posicao " + std::to_string(P) + ": GPU == CPU");
        }

        // ---- REGIAO CURTA: mais comandos no lote do que a regiao reserva ----
        //
        // Tabela montada A MAO: a funcao de producao nao a produziria para esta cena, porque as
        // tabelas dao capacidade 4 ao lote 0. Ela passa pela porta das regioes — que confere
        // estrutura, e nao conhece o frame —, e so a GPU descobre que dois comandos nao cabem numa
        // posicao. O excedente NAO pode ir para a posicao 1, que e do lote 1.
        std::vector<FGpuIndirectRegion> Curta(In.BatchCapacity, FGpuIndirectRegion{ 3u, 0u });
        Curta[0] = { 0u, 1u };
        Curta[1] = { 1u, 2u };
        CheckEq(GpuRegionPreflight(Curta, In.BatchCapacity, 3u), GpuRegionPreflightOk,
                "a regiao curta e estruturalmente valida: a porta nao tem como reprova-la");
        const FSonda::FScatter C = S.ExecutarScatter(In, Curta, LotesCpu, {}, 3u);
        CheckEq(C.Contadores[GpuScatterCounterFaultRegion], 1u,
                "regiao curta: o comando excedente vira falha");
        CheckEq(C.Contadores[GpuScatterCounterEmitted], 2u, "regiao curta: os outros dois sao escritos");
        CheckEq(C.Contadores[GpuScatterCounterFaultCursor] + C.Contadores[GpuScatterCounterFaultCapacity],
                0u, "regiao curta: e nenhuma outra falha");
        Check(MesmoComando(C.Args[0], Esperado(0u, 0u)) || MesmoComando(C.Args[0], Esperado(2u, 0u)),
              "regiao curta: a unica posicao do lote 0 tem um dos dois comandos dele");
        Check(MesmoComando(C.Args[1], Esperado(1u, 1u)),
              "regiao curta: a posicao 1, do lote 1, NAO foi invadida pelo excedente do lote 0");
        Check(MesmoComando(C.Args[2], ComandoPoison()), "regiao curta: a folga do lote 1 continua poison");
        {
            std::vector<FGpuIndirectDrawArgs> ArgsC(3u, ComandoPoison());
            std::vector<u32> CurC(LotesCpu.size(), 0u), ContC;
            ScatterIndirectArgs(In, Curta, LotesCpu, CurC, ArgsC, ContC);
            for (u32 K = 0; K < GpuScatterCounterCount; ++K)
                CheckEq(ContC[K], C.Contadores[K],
                        "regiao curta, contador " + std::to_string(K) + ": CPU == GPU");
        }

        // ---- REGIAO FORA DO RECURSO ----
        //
        // As regioes de producao contra um `IndirectArgs` de CINCO comandos: a regiao 1 = [4, 6) nao
        // cabe. A porta das regioes reprova (conferido aqui), e o shader tem de conter mesmo assim.
        CheckEq(GpuRegionPreflight(Regioes, In.BatchCapacity, TotalReg - 1u),
                GpuRegionPreflightSmallArgsBuffer, "a porta reprova as regioes da cena contra cinco comandos");
        const FSonda::FScatter F = S.ExecutarScatter(In, Regioes, LotesCpu, {}, TotalReg - 1u);
        CheckEq(F.Contadores[GpuScatterCounterFaultCapacity], 1u,
                "regiao fora do recurso: o comando do lote 1 vira falha");
        CheckEq(F.Contadores[GpuScatterCounterEmitted], 2u, "e os do lote 0, que cabe, sao escritos");
        CheckEq(F.Contadores[GpuScatterCounterFaultCursor] + F.Contadores[GpuScatterCounterFaultRegion],
                0u, "regiao fora do recurso: e nenhuma outra falha");
        {
            const FGpuIndirectDrawArgs L0[2]{ Esperado(0u, 0u), Esperado(2u, 0u) };
            const auto Got0 = Ordenados(F.Args.data(), 2u);
            const auto Want0 = Ordenados(L0, 2u);
            Check(MesmoComando(Got0[0], Want0[0]) && MesmoComando(Got0[1], Want0[1]),
                  "regiao fora do recurso: o lote 0 intacto");
            for (const u32 P : { 2u, 3u, 4u })
                Check(MesmoComando(F.Args[P], ComandoPoison()),
                      "regiao fora do recurso: a posicao " + std::to_string(P) + " continua poison");
            std::vector<FGpuIndirectDrawArgs> ArgsF(TotalReg - 1u, ComandoPoison());
            std::vector<u32> CurF(LotesCpu.size(), 0u), ContF;
            ScatterIndirectArgs(In, Regioes, LotesCpu, CurF, ArgsF, ContF);
            for (u32 K = 0; K < GpuScatterCounterCount; ++K)
                CheckEq(ContF[K], F.Contadores[K],
                        "regiao fora do recurso, contador " + std::to_string(K) + ": CPU == GPU");
        }

        // ---- a regiao cujo FIM daria a volta em u32 ----
        //
        // `{0xFFFFFFFF, 2}`: somando antes de comparar, o fim seria 1, e 1 cabe em qualquer buffer. O
        // comando iria para a posicao 0xFFFFFFFF — fora do recurso — ou, com um cursor a mais, para a
        // posicao 0, em cima do lote 0.
        std::vector<FGpuIndirectRegion> Volta = Regioes;
        Volta[1] = { 0xFFFFFFFFu, 2u };
        const FSonda::FScatter V = S.ExecutarScatter(In, Volta, LotesCpu, {}, TotalReg);
        CheckEq(V.Contadores[GpuScatterCounterFaultCapacity], 1u,
                "o fim que daria a volta e falha de capacidade, e nao escrita");
        CheckEq(V.Contadores[GpuScatterCounterEmitted], 2u, "e so o lote 0 e escrito");
        for (const u32 P : { 4u, 5u })
            Check(MesmoComando(V.Args[P], ComandoPoison()),
                  "fim que daria a volta: a posicao " + std::to_string(P) + " continua poison");
        {
            std::vector<FGpuIndirectDrawArgs> ArgsV(TotalReg, ComandoPoison());
            std::vector<u32> CurV(LotesCpu.size(), 0u), ContV;
            ScatterIndirectArgs(In, Volta, LotesCpu, CurV, ArgsV, ContV);
            for (u32 K = 0; K < GpuScatterCounterCount; ++K)
                CheckEq(ContV[K], V.Contadores[K],
                        "fim que daria a volta, contador " + std::to_string(K) + ": CPU == GPU");
        }
    }

    // ---- a CADEIA sobre recursos PERSISTENTES: reset -> classify -> scatter -> finalizacao ----
    //
    // Os executores acima criam buffers novos a cada chamada e por isso nao enxergam um acumulador que
    // atravessa de uma execucao para a seguinte. Aqui a cadeia roda DUAS vezes sobre os mesmos
    // recursos, como o frame faria, e depois uma terceira sem o reset.
    {
        FCadeia C(S, In, Regioes, TotalReg);
        C.Criar();

        const FCadeia::FSaida R1 = C.Rodar(FCadeia::kResetTudo);
        const FCadeia::FSaida R2 = C.Rodar(FCadeia::kResetTudo);

        for (u32 Rodada = 1; Rodada <= 2u; ++Rodada) {
            const FCadeia::FSaida& R = Rodada == 1u ? R1 : R2;
            const std::string N = std::to_string(Rodada) + "a execucao";
            CheckEq(R.Status, 1u, N + ": o frame fecha");
            CheckEq(R.Reason, 0u, N + ": sem motivo de invalidacao");
            for (u32 B = 0; B < In.BatchCapacity; ++B) {
                CheckEq(R.Draw[B], QueridoLote[B], N + ": DrawCounts do lote " + std::to_string(B));
                CheckEq(R.Cursors[B], R.Counts[B], N + ": cursor == contagem no lote " + std::to_string(B));
                // O count buffer nunca alcanca alem da regiao. E isso que torna
                // `MaxCommandCount = Capacity` um teto que nunca corta comando real.
                Check(R.Draw[B] <= Regioes[B].Capacity,
                      N + ": DrawCounts dentro da regiao do lote " + std::to_string(B));
            }
            CheckEq(R.ScatCont[GpuScatterCounterEmitted], 3u, N + ": tres comandos emitidos");
            ConferirArgsDaCena(R.Args, N);
        }

        // As duas execucoes com reset produzem o MESMO resultado. E isto que prova que nada atravessou
        // de uma para a outra — um cursor ou uma contagem acumulada apareceria na segunda.
        for (u32 B = 0; B < In.BatchCapacity; ++B)
            CheckEq(R2.Draw[B], R1.Draw[B], "2a == 1a no DrawCounts do lote " + std::to_string(B));

        // ---- a REDE DE SEGURANCA: terceira execucao SEM reset ----
        //
        // Sem reset os quatro acumuladores crescem EM SINCRONIA: as contagens dobram para {4, 2}, os
        // cursores continuam de {2, 1} e terminam em {4, 2}, os emitidos vao a 6. Toda igualdade ENTRE
        // acumuladores continua valendo — e, com a FOLGA das regioes, a contagem dobrada ainda cabe
        // nelas. So a ANCORA — candidatos desta execucao, vindos do cbuffer — enxerga que os
        // contadores do classify somam 28, e nao 14.
        const FCadeia::FSaida R3 = C.Rodar(0u);
        CheckEq(R3.Counts[0], 4u, "sem reset BatchCounts acumula: 2 + 2");
        CheckEq(R3.Cursors[0], R3.Counts[0],
                "e os cursores acumulam JUNTO — por isso cursor == contagem nao basta");
        Check(R3.Counts[0] <= Regioes[0].Capacity && R3.Counts[1] <= Regioes[1].Capacity,
              "e a contagem dobrada CABE nas regioes — por isso a capacidade tambem nao basta");
        CheckEq(R3.Status, 0u, "sem reset o frame e invalidado");
        CheckEq(R3.Reason, static_cast<u32>(GpuFinalizeClassifyAnchor),
                "e o UNICO motivo e a ancora: sem ela, esquecer o reset inteiro passaria");
        for (u32 B = 0; B < In.BatchCapacity; ++B)
            CheckEq(R3.Draw[B], 0u, "sem reset o count buffer sai zerado no lote " + std::to_string(B));

        // A referencia CPU da finalizacao, sobre os MESMOS numeros lidos da GPU, chega ao mesmo veredito.
        std::vector<u32> DrawCpu; u32 MotivoCpu = 0u;
        const u32 StatusCpu = FinalizeDrawCounts(R3.Counts, Regioes, R3.ScatCont, R3.Cursors,
                                                 R3.ClassCont, Candidatos, DrawCpu, MotivoCpu);
        CheckEq(StatusCpu, R3.Status, "finalizacao CPU == GPU no status");
        CheckEq(MotivoCpu, R3.Reason, "finalizacao CPU == GPU no motivo");

        // ---- cada elo da finalizacao, pela SUA causa: um acumulador sem reset de cada vez ----
        //
        // Antes de cada cenario, uma execucao limpa — que tambem prova que o reset completo recupera
        // de qualquer estado parcial. Os motivos esperados foram calculados a mao a partir do estado
        // que a execucao limpa deixa (contagens {2,1}, cursores {2,1}, 3 emitidos, 14 candidatos,
        // regioes {4,2}):
        //
        //   sem reset dos CURSORES   : comecam em {2,1}, todos os tres passam da contagem
        //                              -> falha de cursor + cursor != contagem
        //   sem reset das CONTAGENS  : viram {4,2} contra 3 indiretos frescos; cabem nas regioes
        //                              -> soma dos lotes + cursor != contagem
        //   sem reset do SCATTER     : 6 emitidos contra 3 indiretos -> soma do scatter
        //   sem reset do CLASSIFY    : 28 contra 14 candidatos, 6 indiretos contra 3 nos lotes e no
        //                              scatter -> ancora + soma dos lotes + soma do scatter
        struct FCenario { const char* Nome; u32 Reset; u32 Motivo; };
        const FCenario Cenarios[] = {
            { "cursores",  FCadeia::kResetTudo & ~FCadeia::kResetCursors,
              GpuFinalizeFaultCursor | GpuFinalizeCursorMismatch },
            { "contagens", FCadeia::kResetTudo & ~FCadeia::kResetCounts,
              GpuFinalizeBatchSum | GpuFinalizeCursorMismatch },
            { "scatter",   FCadeia::kResetTudo & ~FCadeia::kResetScatCont, GpuFinalizeScatterSum },
            { "classify",  FCadeia::kResetTudo & ~FCadeia::kResetClassCont,
              GpuFinalizeClassifyAnchor | GpuFinalizeBatchSum | GpuFinalizeScatterSum },
        };
        for (const FCenario& Cn : Cenarios) {
            const FCadeia::FSaida Limpa = C.Rodar(FCadeia::kResetTudo);
            CheckEq(Limpa.Status, 1u, std::string("reset completo recupera antes de '") + Cn.Nome + "'");
            const FCadeia::FSaida R = C.Rodar(Cn.Reset);
            const std::string N = std::string("sem reset de ") + Cn.Nome;
            CheckEq(R.Status, 0u, N + ": frame invalidado");
            CheckEq(R.Reason, Cn.Motivo, N + ": o motivo exato");
            for (u32 B = 0; B < In.BatchCapacity; ++B)
                CheckEq(R.Draw[B], 0u, N + ": count buffer zerado no lote " + std::to_string(B));
            // A referencia CPU da finalizacao chega ao mesmo motivo sobre os mesmos numeros. Sem isto o
            // espelho seria verificado so no cenario sem reset nenhum, e um elo divergente nos outros
            // passaria.
            std::vector<u32> DCpu; u32 MCpu = 0u;
            FinalizeDrawCounts(R.Counts, Regioes, R.ScatCont, R.Cursors, R.ClassCont, Candidatos,
                               DCpu, MCpu);
            CheckEq(MCpu, R.Reason, N + ": finalizacao CPU == GPU no motivo");
        }

        // ---- REGIOES que a CPU nao aprovaria, entregues a cadeia inteira ----
        //
        // O scatter isolado provou que a defesa CONTEM a escrita. A pergunta aqui e outra: a falha
        // chega ao COUNT BUFFER? Um frame com regiao estourada tem de publicar zero em TODO lote — nao
        // so no que estourou —, com o motivo exato:
        //
        //   regiao curta ({0,1} e {1,2}) : o lote 0 conta 2 numa regiao de 1
        //                                  -> estouro de regiao (finalizacao) + falha de regiao (scatter)
        //   regiao fora do recurso       : regioes de producao, shader recebendo cinco comandos
        //                                  -> falha de capacidade (scatter)
        std::vector<FGpuIndirectRegion> Curta(In.BatchCapacity, FGpuIndirectRegion{ 3u, 0u });
        Curta[0] = { 0u, 1u };
        Curta[1] = { 1u, 2u };
        struct FRegiaoRuim {
            const char*                     Nome;
            std::vector<FGpuIndirectRegion> Tabela;
            u32                             CapacidadeShader;
            u32                             Motivo;
        };
        const FRegiaoRuim Ruins[] = {
            { "regiao curta", Curta, TotalReg, GpuFinalizeRegionOverflow | GpuFinalizeFaultRegion },
            { "regiao fora do recurso", Regioes, TotalReg - 1u, GpuFinalizeFaultCapacity },
        };
        for (const FRegiaoRuim& Rr : Ruins) {
            C.Regioes          = Regioes;
            C.CapacidadeShader = TotalReg;
            const FCadeia::FSaida Limpa = C.Rodar(FCadeia::kResetTudo);
            CheckEq(Limpa.Status, 1u,
                    std::string("regioes de producao fecham antes de '") + Rr.Nome + "'");

            C.Regioes          = Rr.Tabela;
            C.CapacidadeShader = Rr.CapacidadeShader;
            const FCadeia::FSaida R = C.Rodar(FCadeia::kResetTudo);
            const std::string N = Rr.Nome;
            CheckEq(R.Status, 0u, N + ": frame invalidado");
            CheckEq(R.Reason, Rr.Motivo, N + ": o motivo exato");
            for (u32 B = 0; B < In.BatchCapacity; ++B)
                CheckEq(R.Draw[B], 0u, N + ": count buffer zerado no lote " + std::to_string(B));
            std::vector<u32> DCpu; u32 MCpu = 0u;
            FinalizeDrawCounts(R.Counts, Rr.Tabela, R.ScatCont, R.Cursors, R.ClassCont, Candidatos,
                               DCpu, MCpu);
            CheckEq(MCpu, R.Reason, N + ": finalizacao CPU == GPU no motivo");
        }
    }

    // Capacidade curta: a DEFESA de ultima instancia do shader, sob uma entrada que o preflight
    // reprova (TestarPreflight confere que reprova). Ela nao deveria acontecer em producao — quem a
    // impede e o preflight, porque descobrir falta de espaco depois do dispatch ja e geometria
    // perdida naquele frame. Exercitada aqui porque defesa que nenhum teste alcanca e comentario:
    // com capacidade 1, o comando do pool 1 nao tem slot e tem de ser CONTADO, nunca somado no slot
    // errado de outro lote.
    {
        FGpuClassifyInput Curta = In;
        Curta.BatchCapacity = 1u;
        std::vector<u32> LotesC, ContC;
        ClassifyCandidates(Curta, LotesC, ContC);
        const FResultado G = S.Executar(Curta);
        CheckEq(G.Contadores[GpuClassifyOverflow], 1u, "capacidade curta conta overflow na GPU");
        CheckEq(ContC[GpuClassifyOverflow], 1u, "capacidade curta conta overflow na CPU");
        CheckEq(G.Contadores[GpuClassifyIndirect], 2u, "e os outros dois comandos continuam");
        CheckEq(G.Lotes[0], 2u, "o lote que cabe recebe os dois");
        u32 Soma = 0;
        for (const u32 Ct : G.Contadores) Soma += Ct;
        CheckEq(Soma, Candidatos, "overflow tambem e UM desfecho, nao zero");
    }

    // Lista vazia: nenhum dispatch, nenhum contador. Sem este caso, um shader que lesse
    // `ExpandedSlots[0]` fora de guarda so apareceria numa cena sem candidatos.
    {
        FGpuClassifyInput Vazia = In;
        Vazia.Slots = {};
        const FResultado G = S.Executar(Vazia);
        u32 Soma = 0;
        for (const u32 Ct : G.Contadores) Soma += Ct;
        CheckEq(Soma, 0u, "lista vazia nao produz desfecho nenhum");
    }

    if (Failures == 0) {
        std::cout << "  " << Slot.size() << " candidatos classificados em WARP, todos OK\n";
        return 0;
    }
    std::cerr << "  " << Failures << " falha(s)\n";
    return 1;
} catch (const std::exception& E) {
    std::cerr << "Smile.GpuClassifyWarp: excecao -- " << E.what() << '\n';
    return 2;
}
