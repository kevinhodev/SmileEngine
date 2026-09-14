// Classificacao e contagem por lote, executadas em WARP (Fase 5 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir, em ordem de gravidade:
//
//  1. um candidato receber DOIS resultados de classificacao, ou nenhum. A contabilidade e o que
//     permite auditar a fatia; ela NAO prova "desenhado exatamente uma vez" — ver a nota do
//     GpuClassify.h sobre o que a soma prova e o que ela nao prova;
//  2. a leitura de uma linha ANTES de validar o indice. Load fora do StructuredBuffer nao trava —
//     o D3D12 devolve zero —, e zero e a linha do material default e a da malha ausente. O
//     candidato passaria a desenhar algo PLAUSIVEL e errado;
//  3. a porta do indireto ser partida em dois testes. So o predicado completo pode decidir; a
//     decomposicao existe para ATRIBUIR o motivo, e um dia em que ela decidisse mandaria a malha
//     irrepresentavel para o command processor com offset poison;
//  4. os indices dos desfechos divergirem entre C++ e HLSL. Sao dois espelhos de macros e nada os
//     liga em tempo de compilacao; aqui os nove sao lidos de volta.
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
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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


    // Versao u64 do CheckEq. Existe porque `GpuBatchSlotCount` devolve u64 de proposito: comparar o
    // produto depois de espremer em u32 seria testar exatamente o bug que ele evita.
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
    // passaria.
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

    // Doze linhas, uma por situacao. A generation e 1 em todas menos na ultima, que fica VAZIA.
    std::vector<FGpuSceneInstanceMeta> Meta() {
        std::vector<FGpuSceneInstanceMeta> M(12);
        for (auto& R : M) { R.Generation = 1u; R.Flags = GpuSceneInstanceVisible; }
        const u32 Mat[12]  = { 0, 0, 0, 1, 2, 3, 0,  1, 0, 99, 0,  0 };
        const u32 Mesh[12] = { 0, 1, 0, 0, 0, 0, 2,  2, 3,  0, 99, 0 };
        for (u32 I = 0; I < 12u; ++I) { M[I].MaterialId = Mat[I]; M[I].MeshId = Mesh[I]; }
        M[11].Generation = 0u; // linha sem ocupante
        return M;
    }

    // Os catorze candidatos: as doze linhas, mais um buraco do expand e um slot fora da tabela.
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

    // ---- parte CPU pura: o prefixo ------------------------------------------------------------
    //
    // Exclusivo: `Offsets[b]` e onde o lote b comeca, e o total e a soma. O oraculo e escrito a mao,
    // e nao a propria funcao rodada de novo.
    void TestarPrefixo() {
        {
            const u32 Contagens[6]{ 2u, 0u, 5u, 1u, 0u, 3u };
            const u32 Esperado[6]{ 0u, 2u, 2u, 7u, 8u, 8u };
            std::vector<u32> Offsets; u32 Estourou = 0u;
            const u32 Total = PrefixBatchCounts(Contagens, Offsets, Estourou);
            CheckEq(Total, 11u, "o total e a soma das contagens");
            CheckEq(Estourou, 0u, "e nao estourou");
            Check(Offsets.size() == 6, "uma entrada por lote");
            for (u32 I = 0; I < 6u; ++I)
                CheckEq(Offsets[I], Esperado[I], "offset do lote " + std::to_string(I));
            // Lotes VAZIOS nao consomem espaco: o offset do lote 2 e o mesmo do 1. Se o prefixo
            // fosse inclusivo, cada lote apontaria para dentro do seguinte.
            CheckEq(Offsets[1], Offsets[2], "lote vazio nao avanca o offset");
        }
        {
            // Lista vazia: nada a prefixar, total zero, sem tocar em nada.
            std::vector<u32> Offsets{ 7u, 7u }; u32 Estourou = 1u;
            CheckEq(PrefixBatchCounts({}, Offsets, Estourou), 0u, "lista vazia soma zero");
            CheckEq(Estourou, 0u, "e nao marca estouro");
            Check(Offsets.empty(), "e a saida fica vazia, e nao com o conteudo anterior");
        }
        {
            // A volta em u32. Inalcancavel a partir do classify — cada candidato contribui no
            // maximo 1 —, mas o shader nao sabe quem escreveu o buffer que ele soma. Um prefixo que
            // da a volta aponta comandos para dentro dos comandos de outro lote.
            const u32 Contagens[3]{ 0xFFFFFFF0u, 0x20u, 1u };
            std::vector<u32> Offsets; u32 Estourou = 0u;
            PrefixBatchCounts(Contagens, Offsets, Estourou);
            CheckEq(Estourou, 1u, "a soma que daria a volta e marcada");
            CheckEq(Offsets[0], 0u, "a entrada ANTERIOR ao estouro continua correta");
            CheckEq(Offsets[1], kGpuBatchOffsetPoison, "a entrada que FALHOU recebe poison");
            CheckEq(Offsets[2], kGpuBatchOffsetPoison, "e as seguintes tambem");
        }
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

    // ---- parte CPU pura: dimensionamento, preflight e a referencia do scatter ----------------
    void TestarScatterCpu() {
        // Bytes em u64. Em u32, 153.391.690 comandos ja dariam a volta.
        CheckEq64(GpuScatterArgsBytes(14u), 14ull * 28ull, "14 comandos, 28 B cada");
        CheckEq64(GpuScatterArgsBytes(0u), 28ull, "zero comandos ainda aloca um: recurso vazio e ilegal");
        CheckEq64(GpuScatterArgsBytes(0xFFFFFFFFu), 0xFFFFFFFFull * 28ull,
                  "o maior u32 de comandos nao da a volta nos bytes");

        CheckEq(GpuScatterPreflight(14u, 14u), GpuScatterPreflightOk, "capacidade exata passa");
        CheckEq(GpuScatterPreflight(14u, 13u), GpuScatterPreflightSmallArgsBuffer,
                "um comando a menos reprova");
        CheckEq(GpuScatterPreflight(0u, 0u), GpuScatterPreflightOk, "cena vazia passa");

        // A referencia, sobre a cena do teste, com os offsets que o prefixo produz.
        const auto Mats = Materiais(); const auto Meshs = Malhas();
        const auto Metas = Meta();     const auto S = Slots();
        FGpuClassifyInput In{};
        In.Slots = S; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
        In.PoolCount = kPoolCount;
        In.BatchCapacity = CapacidadeDeLote(kPoolCount);

        std::vector<u32> Lotes, ContClassify;
        ClassifyCandidates(In, Lotes, ContClassify);
        std::vector<u32> Offsets; u32 Estourou = 0u;
        const u32 Total = PrefixBatchCounts(Lotes, Offsets, Estourou);

        const u32 Capacidade = static_cast<u32>(S.size());
        std::vector<FGpuIndirectDrawArgs> Args(Capacidade, ComandoPoison());
        std::vector<u32> Cont;
        std::vector<u32> Cursores(Lotes.size(), 0u);
        ScatterIndirectArgs(In, Offsets, Lotes, Estourou, Cursores, Args, Cont);

        // A contagem e a emissao olham o mesmo fato. Um scatter que recusasse alguem que o classify
        // contou apareceria AQUI, antes de virar comando velho no ExecuteIndirect.
        CheckEq(Cont[GpuScatterCounterEmitted], ContClassify[GpuClassifyIndirect],
                "o scatter emite exatamente o que o classify contou");
        CheckEq(Cont[GpuScatterCounterSuppressed], 0u, "sem estouro, nada suprimido");
        CheckEq(Cont[GpuScatterCounterFaultCursor], 0u, "cursores zerados: nenhuma falha de cursor");
        CheckEq(Cont[GpuScatterCounterFaultCapacity], 0u, "preflight aprovado: nenhuma falha de capacidade");

        // Lote 0 (pool 0): slots 0 e 2, os dois na malha 0. Lote 1 (pool 1): slot 1, malha 1.
        const FGpuIndirectDrawArgs L0[2]{ Esperado(0u, 0u), Esperado(2u, 0u) };
        const auto Got0 = Ordenados(&Args[Offsets[0]], Lotes[0]);
        const auto Want0 = Ordenados(L0, 2u);
        Check(Lotes[0] == 2u && Got0.size() == 2 && MesmoComando(Got0[0], Want0[0]) &&
              MesmoComando(Got0[1], Want0[1]), "lote 0: o multiconjunto exato de comandos");
        Check(Lotes[1] == 1u && MesmoComando(Args[Offsets[1]], Esperado(1u, 1u)),
              "lote 1: o comando exato");

        // RESIDUO: toda posicao a partir do total continua poison. Um scatter que escrevesse uma
        // posicao a frente (cursor pos-incremento, por exemplo) sujaria a primeira delas.
        for (u32 P = Total; P < Capacidade; ++P)
            Check(MesmoComando(Args[P], ComandoPoison()),
                  "posicao " + std::to_string(P) + " fora dos lotes continua poison");
    }
}

namespace {
    struct FResultado {
        std::vector<u32> Lotes;
        std::vector<u32> Contadores;
        // Saida do prefixo, produzida no MESMO command list, a partir do BatchCounts que o classify
        // acabou de escrever. Encadear na GPU e o ponto: na producao nao ha readback entre os dois.
        std::vector<u32> Offsets;
        std::vector<u32> PrefixCont;
    };

    // A sonda. Mesma forma da do expand; o que muda sao as views e o CB.
    struct FSonda {
        wrl::ComPtr<ID3D12Device>        Device;
        wrl::ComPtr<ID3D12CommandQueue>  Queue;
        wrl::ComPtr<ID3D12RootSignature> Root;
        wrl::ComPtr<ID3D12RootSignature> RootPrefixo;
        wrl::ComPtr<ID3D12RootSignature> RootScatter;
        wrl::ComPtr<ID3D12RootSignature> RootFinalize;
        wrl::ComPtr<ID3D12PipelineState> Pipeline;
        wrl::ComPtr<ID3D12PipelineState> PipelinePrefixo;
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

        void Construir(const std::vector<u8>& _Bytecode, const std::vector<u8>& _Prefixo,
                       const std::vector<u8>& _Scatter, const std::vector<u8>& _Finalize) {
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

            D3D12_DESCRIPTOR_RANGE Ranges[2]{};
            Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            Ranges[0].NumDescriptors = 4;
            Ranges[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            Ranges[1].NumDescriptors = 2;
            D3D12_ROOT_PARAMETER Params[3]{};
            Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            for (int I = 0; I < 2; ++I) {
                Params[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                Params[I + 1].DescriptorTable.NumDescriptorRanges = 1;
                Params[I + 1].DescriptorTable.pDescriptorRanges   = &Ranges[I];
            }
            D3D12_ROOT_SIGNATURE_DESC SigDesc{};
            SigDesc.NumParameters = 3;
            SigDesc.pParameters   = Params;
            wrl::ComPtr<ID3DBlob> Serial, Erro;
            Hr(D3D12SerializeRootSignature(&SigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Serial, &Erro),
               "SerializeRootSignature");
            Hr(Device->CreateRootSignature(0, Serial->GetBufferPointer(), Serial->GetBufferSize(),
                                           IID_PPV_ARGS(&Root)), "CreateRootSignature");

            D3D12_COMPUTE_PIPELINE_STATE_DESC PSO{};
            PSO.pRootSignature = Root.Get();
            PSO.CS             = { _Bytecode.data(), _Bytecode.size() };
            Hr(Device->CreateComputePipelineState(&PSO, IID_PPV_ARGS(&Pipeline)),
               "CreateComputePipelineState");

            // O prefixo tem root signature PROPRIA: uma SRV e duas UAVs (offsets e contadores).
            // Reaproveitar a do classify
            // deixaria tres SRVs do range ligados sem descritor inicializado, que e coisa que a
            // camada de debug reclama e que nao tem nada a ver com o que esta sob teste.
            D3D12_DESCRIPTOR_RANGE RangesP[2]{};
            RangesP[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            RangesP[0].NumDescriptors = 1;
            RangesP[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            RangesP[1].NumDescriptors = 2; // BatchOffsets, Counters
            D3D12_ROOT_PARAMETER ParamsP[3]{};
            ParamsP[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            for (int I = 0; I < 2; ++I) {
                ParamsP[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                ParamsP[I + 1].DescriptorTable.NumDescriptorRanges = 1;
                ParamsP[I + 1].DescriptorTable.pDescriptorRanges   = &RangesP[I];
            }
            D3D12_ROOT_SIGNATURE_DESC SigDescP{};
            SigDescP.NumParameters = 3;
            SigDescP.pParameters   = ParamsP;
            wrl::ComPtr<ID3DBlob> SerialP, ErroP;
            Hr(D3D12SerializeRootSignature(&SigDescP, D3D_ROOT_SIGNATURE_VERSION_1, &SerialP,
                                           &ErroP), "SerializeRootSignature (prefixo)");
            Hr(Device->CreateRootSignature(0, SerialP->GetBufferPointer(), SerialP->GetBufferSize(),
                                           IID_PPV_ARGS(&RootPrefixo)),
               "CreateRootSignature (prefixo)");
            D3D12_COMPUTE_PIPELINE_STATE_DESC PSOP{};
            PSOP.pRootSignature = RootPrefixo.Get();
            PSOP.CS             = { _Prefixo.data(), _Prefixo.size() };
            Hr(Device->CreateComputePipelineState(&PSOP, IID_PPV_ARGS(&PipelinePrefixo)),
               "CreateComputePipelineState (prefixo)");

            // O scatter: seis SRVs (slots + tres tabelas + offsets + contadores do prefixo) e tres
            // UAVs (argumentos, cursores, contadores).
            D3D12_DESCRIPTOR_RANGE RangesS[2]{};
            RangesS[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            RangesS[0].NumDescriptors = 7;
            RangesS[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            RangesS[1].NumDescriptors = 3;
            D3D12_ROOT_PARAMETER ParamsS[3]{};
            ParamsS[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            for (int I = 0; I < 2; ++I) {
                ParamsS[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                ParamsS[I + 1].DescriptorTable.NumDescriptorRanges = 1;
                ParamsS[I + 1].DescriptorTable.pDescriptorRanges   = &RangesS[I];
            }
            D3D12_ROOT_SIGNATURE_DESC SigDescS{};
            SigDescS.NumParameters = 3;
            SigDescS.pParameters   = ParamsS;
            wrl::ComPtr<ID3DBlob> SerialS, ErroS;
            Hr(D3D12SerializeRootSignature(&SigDescS, D3D_ROOT_SIGNATURE_VERSION_1, &SerialS,
                                           &ErroS), "SerializeRootSignature (scatter)");
            Hr(Device->CreateRootSignature(0, SerialS->GetBufferPointer(), SerialS->GetBufferSize(),
                                           IID_PPV_ARGS(&RootScatter)),
               "CreateRootSignature (scatter)");
            D3D12_COMPUTE_PIPELINE_STATE_DESC PSOS{};
            PSOS.pRootSignature = RootScatter.Get();
            PSOS.CS             = { _Scatter.data(), _Scatter.size() };
            Hr(Device->CreateComputePipelineState(&PSOS, IID_PPV_ARGS(&PipelineScatter)),
               "CreateComputePipelineState (scatter)");

            // A finalizacao: cinco SRVs (contagens, contadores do prefixo, contadores do scatter,
            // cursores, contadores do classify) e duas UAVs (count buffer, status).
            D3D12_DESCRIPTOR_RANGE RangesF[2]{};
            RangesF[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            RangesF[0].NumDescriptors = 5;
            RangesF[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            RangesF[1].NumDescriptors = 2;
            D3D12_ROOT_PARAMETER ParamsF[3]{};
            ParamsF[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            for (int I = 0; I < 2; ++I) {
                ParamsF[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                ParamsF[I + 1].DescriptorTable.NumDescriptorRanges = 1;
                ParamsF[I + 1].DescriptorTable.pDescriptorRanges   = &RangesF[I];
            }
            D3D12_ROOT_SIGNATURE_DESC SigDescF{};
            SigDescF.NumParameters = 3;
            SigDescF.pParameters   = ParamsF;
            wrl::ComPtr<ID3DBlob> SerialF, ErroF;
            Hr(D3D12SerializeRootSignature(&SigDescF, D3D_ROOT_SIGNATURE_VERSION_1, &SerialF,
                                           &ErroF), "SerializeRootSignature (finalizacao)");
            Hr(Device->CreateRootSignature(0, SerialF->GetBufferPointer(), SerialF->GetBufferSize(),
                                           IID_PPV_ARGS(&RootFinalize)),
               "CreateRootSignature (finalizacao)");
            D3D12_COMPUTE_PIPELINE_STATE_DESC PSOF{};
            PSOF.pRootSignature = RootFinalize.Get();
            PSOF.CS             = { _Finalize.data(), _Finalize.size() };
            Hr(Device->CreateComputePipelineState(&PSOF, IID_PPV_ARGS(&PipelineFinalize)),
               "CreateComputePipelineState (finalizacao)");

            D3D12_COMMAND_QUEUE_DESC QD{};
            Hr(Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&Queue)), "CreateCommandQueue");
            Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence");
            Step = Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            Drenar("criacao");
        }


        struct FPrefixo {
            std::vector<u32> Offsets;
            std::vector<u32> Contadores;
        };

        // Execucao ISOLADA do prefixo, com `BatchCounts` semeado a mao.
        //
        // Existe porque o fluxo encadeado nunca consegue estourar: as contagens vem do classify, e a
        // soma delas e limitada pelo numero de candidatos. Sem esta porta de entrada, quebrar a
        // protecao de estouro no HLSL continuaria verde — a defesa viveria so no comentario.
        //
        // O buffer de offsets nasce com um padrao RECONHECIVEL, e nao com zeros: assim o teste
        // distingue "o shader escreveu poison" de "o shader nao escreveu nada". Com zeros, uma
        // implementacao que simplesmente parasse de escrever passaria pelo mesmo teste.
        FPrefixo ExecutarPrefixo(std::span<const u32> _Counts) {
            const u32 Slots = static_cast<u32>(_Counts.size());
            const u64 OffBytes  = std::max<u64>(sizeof(u32), Slots * sizeof(u32));
            const u64 ContBytes = GpuPrefixCounterCount * sizeof(u32);

            auto BufCounts = Upload(_Counts.data(), OffBytes);
            auto BufOffsets = Buffer(D3D12_HEAP_TYPE_DEFAULT, OffBytes, true,
                                     D3D12_RESOURCE_STATE_COMMON);
            auto BufCont = Buffer(D3D12_HEAP_TYPE_DEFAULT, ContBytes, true,
                                  D3D12_RESOURCE_STATE_COMMON);

            const std::vector<u32> Semente(static_cast<size_t>(OffBytes / 4), 0xA5A5A5A5u);
            const u32 ZerosCont[GpuPrefixCounterCount]{};
            auto BufSemente = Upload(Semente.data(), OffBytes);
            auto BufZeros   = Upload(ZerosCont, ContBytes);

            auto LeituraOff  = Buffer(D3D12_HEAP_TYPE_READBACK, OffBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraCont = Buffer(D3D12_HEAP_TYPE_READBACK, ContBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);

            wrl::ComPtr<ID3D12DescriptorHeap> Heap;
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = 3;
            HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Hr(Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");
            auto CpuAt = [&](UINT I) {
                auto H = Heap->GetCPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<SIZE_T>(I) * Step;
                return H;
            };
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max(1u, Slots);
                D.Buffer.StructureByteStride = sizeof(u32);
                Device->CreateShaderResourceView(BufCounts.Get(), &D, CpuAt(0));
            }
            auto UAV = [&](ID3D12Resource* R, u32 N, UINT Slot) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max(1u, N);
                D.Buffer.StructureByteStride = sizeof(u32);
                Device->CreateUnorderedAccessView(R, nullptr, &D, CpuAt(Slot));
            };
            UAV(BufOffsets.Get(), Slots, 1);
            UAV(BufCont.Get(), GpuPrefixCounterCount, 2);

            alignas(16) u8 CB[256]{};
            const u32 P4[4]{ Slots, 0u, 0u, 0u };
            std::memcpy(CB, P4, sizeof(P4));
            auto BufCB = Upload(CB, sizeof(CB));

            wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
            wrl::ComPtr<ID3D12GraphicsCommandList> List;
            Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&Allocator)), "CreateCommandAllocator");
            Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                         PipelinePrefixo.Get(), IID_PPV_ARGS(&List)),
               "CreateCommandList");
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
            List->CopyBufferRegion(BufOffsets.Get(), 0, BufSemente.Get(), 0, OffBytes);
            List->CopyBufferRegion(BufCont.Get(), 0, BufZeros.Get(), 0, ContBytes);
            Transicao(BufOffsets.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ID3D12DescriptorHeap* Heaps[] = { Heap.Get() };
            List->SetDescriptorHeaps(1, Heaps);
            List->SetComputeRootSignature(RootPrefixo.Get());
            List->SetComputeRootConstantBufferView(0, BufCB->GetGPUVirtualAddress());
            auto GpuAt = [&](UINT I) {
                auto H = Heap->GetGPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<UINT64>(I) * Step;
                return H;
            };
            List->SetComputeRootDescriptorTable(1, GpuAt(0));
            List->SetComputeRootDescriptorTable(2, GpuAt(1));
            List->Dispatch(1, 1, 1);

            Transicao(BufOffsets.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraOff.Get(), BufOffsets.Get());
            List->CopyResource(LeituraCont.Get(), BufCont.Get());
            Hr(List->Close(), "Close (prefixo)");

            ID3D12CommandList* Listas[] = { List.Get() };
            Queue->ExecuteCommandLists(1, Listas);
            ++FenceValue;
            Hr(Queue->Signal(Fence.Get(), FenceValue), "Signal (prefixo)");
            if (Fence->GetCompletedValue() < FenceValue) {
                struct FEvent {
                    HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    ~FEvent() { if (Value) CloseHandle(Value); }
                } Evento;
                if (!Evento.Value) Hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
                Hr(Fence->SetEventOnCompletion(FenceValue, Evento.Value), "SetEventOnCompletion");
                if (WaitForSingleObject(Evento.Value, 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error("dispatch de prefixo nao terminou");
            }
            Drenar("prefixo isolado");

            FPrefixo Saida;
            Saida.Offsets.resize(Slots);
            Saida.Contadores.resize(GpuPrefixCounterCount);
            if (Slots > 0) {
                const u32* P = nullptr;
                Hr(LeituraOff->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u32**>(&P))),
                   "Map offsets isolados");
                std::memcpy(Saida.Offsets.data(), P, Slots * sizeof(u32));
                LeituraOff->Unmap(0, nullptr);
            }
            {
                const u32* P = nullptr;
                Hr(LeituraCont->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u32**>(&P))),
                   "Map contadores isolados");
                std::memcpy(Saida.Contadores.data(), P, ContBytes);
                LeituraCont->Unmap(0, nullptr);
            }
            return Saida;
        }

        struct FScatter {
            std::vector<FGpuIndirectDrawArgs> Args;
            std::vector<u32>                  Contadores;
        };

        // O scatter ISOLADO, com offsets e flag do prefixo fornecidos pelo teste.
        //
        // Isolado porque so assim da para forcar `PrefixOverflow = 1` sobre contagens validas — o
        // fluxo encadeado nunca estoura. `IndirectArgs` nasce com o comando POISON em toda posicao:
        // uma posicao que o scatter deixou de escrever aparece como poison, e uma escrita fora do
        // lote suja uma posicao que devia continuar poison.
        FScatter ExecutarScatter(const FGpuClassifyInput& _In, std::span<const u32> _Offsets,
                                 std::span<const u32> _Counts, std::span<const u32> _Cursores,
                                 u32 _PrefixOverflow, u32 _ArgsCapacity) {
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
            auto BufOff   = Upload(_Offsets.data(), Pisa(_Offsets.size() * 4u, 4u));
            const u32 PrefCont[GpuPrefixCounterCount]{ 0u, _PrefixOverflow };
            auto BufPref  = Upload(PrefCont, sizeof(PrefCont));
            auto BufCounts = Upload(_Counts.data(), Pisa(_Counts.size() * 4u, 4u));

            // Pela porta de dimensionamento, e nunca `_ArgsCapacity * 28` em u32.
            const u64 ArgsBytes = GpuScatterArgsBytes(_ArgsCapacity);
            const u64 ContBytes = GpuScatterCounterCount * sizeof(u32);
            const u64 CurBytes  = Pisa(_Offsets.size() * 4u, 4u);
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
            // Os cursores INICIAIS vem do teste, e nao de zeros: e assim que se reproduz o cursor sujo
            // de uma execucao anterior sem reset, que a falha de cursor e a finalizacao existem para
            // pegar.
            auto BufSemCur = Upload(CursoresSeed.data(), CurBytes);
            auto BufZerosCont = Upload(ZerosCont, ContBytes);

            auto LeituraArgs = Buffer(D3D12_HEAP_TYPE_READBACK, ArgsBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraCont = Buffer(D3D12_HEAP_TYPE_READBACK, ContBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);

            wrl::ComPtr<ID3D12DescriptorHeap> Heap;
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = 10;
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
            SRV(BufOff.Get(), _Offsets.size(), sizeof(u32), 4);
            SRV(BufPref.Get(), GpuPrefixCounterCount, sizeof(u32), 5);
            SRV(BufCounts.Get(), _Counts.size(), sizeof(u32), 6);
            UAV(BufArgs.Get(), NCmd, sizeof(FGpuIndirectDrawArgs), 7);
            UAV(BufCur.Get(), static_cast<u32>(CurBytes / 4), sizeof(u32), 8);
            UAV(BufCont.Get(), GpuScatterCounterCount, sizeof(u32), 9);

            alignas(16) u8 CB[256]{};
            const u32 P8[8]{ static_cast<u32>(_In.Slots.size()), static_cast<u32>(_In.Meta.size()),
                             static_cast<u32>(_In.Materials.size()),
                             static_cast<u32>(_In.Meshes.size()), _In.PoolCount, NCmd, 0u, 0u };
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
            List->SetComputeRootDescriptorTable(2, GpuAt(7));
            const u32 Total = static_cast<u32>(_In.Slots.size());
            if (Total > 0) List->Dispatch((Total + 63u) / 64u, 1, 1);

            Transicao(BufArgs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraArgs.Get(), BufArgs.Get());
            List->CopyResource(LeituraCont.Get(), BufCont.Get());
            Hr(List->Close(), "Close (scatter)");

            ID3D12CommandList* Listas[] = { List.Get() };
            Queue->ExecuteCommandLists(1, Listas);
            ++FenceValue;
            Hr(Queue->Signal(Fence.Get(), FenceValue), "Signal (scatter)");
            if (Fence->GetCompletedValue() < FenceValue) {
                struct FEvent {
                    HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    ~FEvent() { if (Value) CloseHandle(Value); }
                } Evento;
                if (!Evento.Value) Hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
                Hr(Fence->SetEventOnCompletion(FenceValue, Evento.Value), "SetEventOnCompletion");
                if (WaitForSingleObject(Evento.Value, 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error("dispatch de scatter nao terminou");
            }
            Drenar("scatter");

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

            // Buffers do PREFIXO. Criados aqui junto dos outros para que todas as copias de zeros
            // aconteçam antes de qualquer dispatch.
            const u64 PrefContBytes = GpuPrefixCounterCount * sizeof(u32);
            auto BufOffsets  = Buffer(D3D12_HEAP_TYPE_DEFAULT, LoteBytes, true,
                                      D3D12_RESOURCE_STATE_COMMON);
            auto BufPrefCont = Buffer(D3D12_HEAP_TYPE_DEFAULT, PrefContBytes, true,
                                      D3D12_RESOURCE_STATE_COMMON);
            const u32 ZerosPref[GpuPrefixCounterCount]{};
            auto BufZerosPref = Upload(ZerosPref, PrefContBytes);
            auto LeituraOffsets  = Buffer(D3D12_HEAP_TYPE_READBACK, LoteBytes, false,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraPrefCont = Buffer(D3D12_HEAP_TYPE_READBACK, PrefContBytes, false,
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
            // Do prefixo: o MESMO BufLotes, agora como SRV, mais as duas saidas.
            SRV(BufLotes.Get(), _In.BatchCapacity, sizeof(u32), 6);
            UAV(BufOffsets.Get(), _In.BatchCapacity, 7);
            UAV(BufPrefCont.Get(), GpuPrefixCounterCount, 8);

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
            List->CopyBufferRegion(BufOffsets.Get(), 0, BufZerosLote.Get(), 0, LoteBytes);
            List->CopyBufferRegion(BufPrefCont.Get(), 0, BufZerosPref.Get(), 0, PrefContBytes);
            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufOffsets.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufPrefCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
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

            // ---- prefixo, NO MESMO command list -------------------------------------------------
            //
            // Encadeado na GPU porque e assim que o frame vai fazer: `BatchCounts` e escrito pelo
            // classify e lido pelo prefixo sem nenhum readback no meio.
            //
            // A transicao UAV -> NON_PIXEL_SHADER_RESOURCE e o contrato da secao 6.4 do plano e e
            // ela que serve de ponto de sincronizacao entre os dois dispatches.
            //
            // ⚠️ Este teste NAO prova que ela e necessaria, e eu verifiquei: removendo-a, tudo
            // continua verde. Dois motivos, os dois do ambiente e nao do codigo — buffers em D3D12
            // tem promocao e decaimento implicitos de estado, entao a camada de debug nao reprova a
            // leitura por SRV de um buffer "em" UAV; e o WARP completa o primeiro dispatch antes do
            // segundo, o que esconde a corrida. A ausencia da barreira e uma CORRIDA, e corrida nao
            // se prova por teste deterministico. Ela fica porque o contrato exige, nao porque algo
            // aqui a exija — e essa distincao esta escrita para nao ser confundida depois.
            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            alignas(16) u8 CBP[256]{};
            const u32 P4[4]{ _In.BatchCapacity, 0u, 0u, 0u };
            std::memcpy(CBP, P4, sizeof(P4));
            auto BufCBP = Upload(CBP, sizeof(CBP));

            List->SetPipelineState(PipelinePrefixo.Get());
            List->SetComputeRootSignature(RootPrefixo.Get());
            List->SetComputeRootConstantBufferView(0, BufCBP->GetGPUVirtualAddress());
            List->SetComputeRootDescriptorTable(1, GpuAt(6));
            List->SetComputeRootDescriptorTable(2, GpuAt(7));
            List->Dispatch(1, 1, 1);

            Transicao(BufOffsets.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufPrefCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraOffsets.Get(), BufOffsets.Get());
            List->CopyResource(LeituraPrefCont.Get(), BufPrefCont.Get());

            // `BufLotes` ja saiu de UAV na transicao para o prefixo: parte de
            // NON_PIXEL_SHADER_RESOURCE. Um `StateBefore` errado aqui e recusado pela camada de
            // debug, que este teste trata como reprovacao.
            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraLotes.Get(), BufLotes.Get());
            List->CopyResource(LeituraCont.Get(), BufCont.Get());
            Hr(List->Close(), "Close");

            ID3D12CommandList* Listas[] = { List.Get() };
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
                    throw std::runtime_error("dispatch de classificacao nao terminou");
            }
            Drenar("dispatch");

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
            Saida.Offsets.resize(_In.BatchCapacity);
            Saida.PrefixCont.resize(GpuPrefixCounterCount);
            if (_In.BatchCapacity > 0) {
                const u32* P = nullptr;
                Hr(LeituraOffsets->Map(0, nullptr,
                                       reinterpret_cast<void**>(const_cast<u32**>(&P))),
                   "Map offsets");
                std::memcpy(Saida.Offsets.data(), P, _In.BatchCapacity * sizeof(u32));
                LeituraOffsets->Unmap(0, nullptr);
            }
            {
                const u32* P = nullptr;
                Hr(LeituraPrefCont->Map(0, nullptr,
                                        reinterpret_cast<void**>(const_cast<u32**>(&P))),
                   "Map cont prefixo");
                std::memcpy(Saida.PrefixCont.data(), P, PrefContBytes);
                LeituraPrefCont->Unmap(0, nullptr);
            }
            return Saida;
        }
    };
}

namespace {
    // A CADEIA inteira, sobre recursos PERSISTENTES:
    //
    //     reset -> classify -> prefixo -> scatter -> finalizacao
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
        u32                      Capacidade = 0; // comandos em IndirectArgs

        wrl::ComPtr<ID3D12Resource> BufSlots, BufMeta, BufMats, BufMesh, BufZeros;
        wrl::ComPtr<ID3D12Resource> Counts, ClassCont, Offsets, PrefCont;
        wrl::ComPtr<ID3D12Resource> Args, Cursors, ScatCont, Draw, Status;
        wrl::ComPtr<ID3D12DescriptorHeap> Heap;
        std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> Estados;

        struct FSaida {
            std::vector<u32>                  Counts, Cursors, ScatCont, PrefCont, Draw, ClassCont;
            std::vector<FGpuIndirectDrawArgs> Args;
            u32                               Status = 0;
            u32                               Reason = 0;
        };

        FCadeia(FSonda& _S, const FGpuClassifyInput& _In, u32 _Capacidade)
            : S(_S), In(_In), Slots(_In.BatchCapacity), Capacidade(_Capacidade) {}

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
            Offsets   = Uav(LoteB);
            PrefCont  = Uav(GpuPrefixCounterCount * 4u);
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

        // Grava, submete, espera a fence e drena a camada de debug. Uma command list por chamada.
        template <typename TGravar>
        void Executar(TGravar&& _Gravar, const char* _Quando) {
            wrl::ComPtr<ID3D12CommandAllocator>    Alloc;
            wrl::ComPtr<ID3D12GraphicsCommandList> L;
            Hr(S.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&Alloc)), "CreateCommandAllocator");
            Hr(S.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&L)), "CreateCommandList (cadeia)");
            _Gravar(L.Get());
            Hr(L->Close(), "Close (cadeia)");
            ID3D12CommandList* Listas[] = { L.Get() };
            S.Queue->ExecuteCommandLists(1, Listas);
            ++S.FenceValue;
            Hr(S.Queue->Signal(S.Fence.Get(), S.FenceValue), "Signal (cadeia)");
            if (S.Fence->GetCompletedValue() < S.FenceValue) {
                struct FEvent {
                    HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    ~FEvent() { if (Value) CloseHandle(Value); }
                } Ev;
                if (!Ev.Value) Hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
                Hr(S.Fence->SetEventOnCompletion(S.FenceValue, Ev.Value), "SetEventOnCompletion");
                if (WaitForSingleObject(Ev.Value, 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error("cadeia nao terminou");
            }
            S.Drenar(_Quando);
        }

        // Tabelas de descritores, uma por passo, na ordem das root signatures de cada um.
        //   classify: SRV 0-3 (slots, meta, mats, mesh)          UAV 4-5 (counts, classcont)
        //   prefixo:  SRV 6   (counts)                           UAV 7-8 (offsets, prefcont)
        //   scatter:  SRV 9-15 (slots, meta, mats, mesh, offsets, prefcont, counts)
        //                                                        UAV 16-18 (args, cursors, scatcont)
        //   final:    SRV 19-23 (counts, prefcont, scatcont, cursors, classcont) UAV 24-25 (draw, status)
        static constexpr UINT kClassSrv = 0, kClassUav = 4, kPrefSrv = 6, kPrefUav = 7;
        static constexpr UINT kScatSrv = 9, kScatUav = 16, kFinSrv = 19, kFinUav = 24;

        void CriarDescritores() {
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = 26;
            HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Hr(S.Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");
            auto At = [&](UINT I) {
                auto H = Heap->GetCPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<SIZE_T>(I) * S.Step;
                return H;
            };
            auto Srv = [&](ID3D12Resource* R, u64 N, u32 Stride, UINT I) {
                D3D12_SHADER_RESOURCE_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max<u32>(1u, static_cast<u32>(N));
                D.Buffer.StructureByteStride = Stride;
                S.Device->CreateShaderResourceView(R, &D, At(I));
            };
            auto Uav = [&](ID3D12Resource* R, u64 N, u32 Stride, UINT I) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max<u32>(1u, static_cast<u32>(N));
                D.Buffer.StructureByteStride = Stride;
                S.Device->CreateUnorderedAccessView(R, nullptr, &D, At(I));
            };
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
            // prefixo
            Srv(Counts.Get(), Slots, 4u, kPrefSrv);
            Uav(Offsets.Get(), Slots, 4u, kPrefUav + 0);
            Uav(PrefCont.Get(), GpuPrefixCounterCount, 4u, kPrefUav + 1);
            // scatter
            Srv(BufSlots.Get(), In.Slots.size(), 4u, kScatSrv + 0);
            Srv(BufMeta.Get(), In.Meta.size(), MetaStride, kScatSrv + 1);
            Srv(BufMats.Get(), In.Materials.size(), MatStride, kScatSrv + 2);
            Srv(BufMesh.Get(), In.Meshes.size(), MeshStride, kScatSrv + 3);
            Srv(Offsets.Get(), Slots, 4u, kScatSrv + 4);
            Srv(PrefCont.Get(), GpuPrefixCounterCount, 4u, kScatSrv + 5);
            Srv(Counts.Get(), Slots, 4u, kScatSrv + 6);
            Uav(Args.Get(), NCmd, sizeof(FGpuIndirectDrawArgs), kScatUav + 0);
            Uav(Cursors.Get(), Slots, 4u, kScatUav + 1);
            Uav(ScatCont.Get(), GpuScatterCounterCount, 4u, kScatUav + 2);
            // finalizacao
            Srv(Counts.Get(), Slots, 4u, kFinSrv + 0);
            Srv(PrefCont.Get(), GpuPrefixCounterCount, 4u, kFinSrv + 1);
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
            auto CbPref  = Cb({ Slots });
            auto CbScat  = Cb({ Total, static_cast<u32>(In.Meta.size()),
                                static_cast<u32>(In.Materials.size()),
                                static_cast<u32>(In.Meshes.size()), In.PoolCount, NCmd });
            auto CbFin   = Cb({ Slots, Total });

            const u64 LoteB = std::max<u64>(4u, Slots * 4u);
            auto Leitura = [&](u64 _B) {
                return S.Buffer(D3D12_HEAP_TYPE_READBACK, _B, false, D3D12_RESOURCE_STATE_COPY_DEST);
            };
            auto LCounts = Leitura(LoteB), LCursors = Leitura(LoteB), LDraw = Leitura(LoteB);
            auto LScat   = Leitura(GpuScatterCounterCount * 4u);
            auto LPref   = Leitura(GpuPrefixCounterCount * 4u);
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

                // 2. prefixo
                Ir(L, Counts.Get(), SRV_); Ir(L, Offsets.Get(), UAV_); Ir(L, PrefCont.Get(), UAV_);
                L->SetPipelineState(S.PipelinePrefixo.Get());
                L->SetComputeRootSignature(S.RootPrefixo.Get());
                L->SetComputeRootConstantBufferView(0, CbPref->GetGPUVirtualAddress());
                L->SetComputeRootDescriptorTable(1, GpuAt(kPrefSrv));
                L->SetComputeRootDescriptorTable(2, GpuAt(kPrefUav));
                L->Dispatch(1, 1, 1);

                // 3. scatter
                Ir(L, Offsets.Get(), SRV_); Ir(L, PrefCont.Get(), SRV_);
                Ir(L, Args.Get(), UAV_); Ir(L, Cursors.Get(), UAV_); Ir(L, ScatCont.Get(), UAV_);
                L->SetPipelineState(S.PipelineScatter.Get());
                L->SetComputeRootSignature(S.RootScatter.Get());
                L->SetComputeRootConstantBufferView(0, CbScat->GetGPUVirtualAddress());
                L->SetComputeRootDescriptorTable(1, GpuAt(kScatSrv));
                L->SetComputeRootDescriptorTable(2, GpuAt(kScatUav));
                if (Total > 0) L->Dispatch((Total + 63u) / 64u, 1, 1);

                // 4. finalizacao
                Ir(L, ScatCont.Get(), SRV_); Ir(L, Cursors.Get(), SRV_); Ir(L, ClassCont.Get(), SRV_);
                Ir(L, Draw.Get(), UAV_); Ir(L, Status.Get(), UAV_);
                L->SetPipelineState(S.PipelineFinalize.Get());
                L->SetComputeRootSignature(S.RootFinalize.Get());
                L->SetComputeRootConstantBufferView(0, CbFin->GetGPUVirtualAddress());
                L->SetComputeRootDescriptorTable(1, GpuAt(kFinSrv));
                L->SetComputeRootDescriptorTable(2, GpuAt(kFinUav));
                L->Dispatch(1, 1, 1);

                // 5. leitura
                const auto CS = D3D12_RESOURCE_STATE_COPY_SOURCE;
                for (ID3D12Resource* R : { Counts.Get(), Cursors.Get(), Draw.Get(), ScatCont.Get(),
                                           ClassCont.Get(),
                                           PrefCont.Get(), Status.Get(), Args.Get() })
                    Ir(L, R, CS);
                L->CopyResource(LCounts.Get(), Counts.Get());
                L->CopyResource(LCursors.Get(), Cursors.Get());
                L->CopyResource(LDraw.Get(), Draw.Get());
                L->CopyResource(LScat.Get(), ScatCont.Get());
                L->CopyResource(LPref.Get(), PrefCont.Get());
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
            O.PrefCont = Ler(LPref.Get(), GpuPrefixCounterCount * 4u);
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
    TestarPrefixo();
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
    const std::vector<u8> BytecodePrefixo = LerArquivo(SMILE_PREFIX_BATCH_COUNTS_CSO);
    Check(!BytecodePrefixo.empty(), "a sonda do prefixo foi encontrada");
    if (BytecodePrefixo.empty()) {
        std::cerr << "  (esperado em " << SMILE_PREFIX_BATCH_COUNTS_CSO << ")\n";
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
    S.Construir(Bytecode, BytecodePrefixo, BytecodeScatter, BytecodeFinalize);

    const auto Mats = Materiais(); const auto Meshs = Malhas();
    const auto Metas = Meta();     const auto Slot = Slots();

    FGpuClassifyInput In{};
    In.Slots = Slot; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
    In.PoolCount = kPoolCount;
        In.BatchCapacity = CapacidadeDeLote(kPoolCount);

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
    CheckEq(SomaGpu, static_cast<u32>(Slot.size()),
            "na GPU tambem: um desfecho por candidato, nem mais nem menos");

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


    // ---- SCATTER em WARP --------------------------------------------------------------------
    //
    // `IndirectArgs` nasce com o comando POISON em toda posicao. Duas coisas ficam visiveis assim:
    // BURACO (posicao reservada que o scatter nao escreveu: poison dentro de um lote) e RESIDUO
    // (escrita fora do lote: comando onde devia continuar poison). A comparacao dentro do lote e por
    // MULTICONJUNTO, porque a ordem la depende de qual thread chega primeiro ao cursor.
    {
        std::vector<u32> OffCpu; u32 EstCpu = 0u;
        const u32 TotalCmd = PrefixBatchCounts(LotesCpu, OffCpu, EstCpu);
        const u32 Capacidade = static_cast<u32>(Slot.size());
        CheckEq(GpuScatterPreflight(Capacidade, Capacidade), GpuScatterPreflightOk,
                "a capacidade do teste passa pelo preflight do scatter");

        const FSonda::FScatter G = S.ExecutarScatter(In, OffCpu, LotesCpu, {}, 0u, Capacidade);

        CheckEq(G.Contadores[GpuScatterCounterEmitted], Gpu.Contadores[GpuClassifyIndirect],
                "scatter na GPU emite exatamente o que o classify contou");
        CheckEq(G.Contadores[GpuScatterCounterSuppressed], 0u, "sem estouro, nada suprimido");
        CheckEq(G.Contadores[GpuScatterCounterFaultCursor], 0u, "cursores zerados: sem falha de cursor");
        CheckEq(G.Contadores[GpuScatterCounterFaultCapacity], 0u, "preflight aprovado: sem falha de capacidade");

        // Oraculo escrito A MAO: lote 0 = slots 0 e 2 na malha 0; lote 1 = slot 1 na malha 1.
        const FGpuIndirectDrawArgs L0[2]{ Esperado(0u, 0u), Esperado(2u, 0u) };
        const auto Got0  = Ordenados(&G.Args[OffCpu[0]], LotesCpu[0]);
        const auto Want0 = Ordenados(L0, 2u);
        Check(LotesCpu[0] == 2u && Got0.size() == 2 && MesmoComando(Got0[0], Want0[0]) &&
              MesmoComando(Got0[1], Want0[1]),
              "GPU, lote 0: o multiconjunto exato (dois comandos, ordem livre)");
        Check(LotesCpu[1] == 1u && MesmoComando(G.Args[OffCpu[1]], Esperado(1u, 1u)),
              "GPU, lote 1: o comando exato");

        // Nenhum buraco: toda posicao reservada deixou de ser poison.
        for (u32 P = 0; P < TotalCmd; ++P)
            Check(!MesmoComando(G.Args[P], ComandoPoison()),
                  "posicao reservada " + std::to_string(P) + " foi escrita (sem buraco)");
        // Nenhum residuo: toda posicao a partir do total continua poison.
        for (u32 P = TotalCmd; P < Capacidade; ++P)
            Check(MesmoComando(G.Args[P], ComandoPoison()),
                  "posicao " + std::to_string(P) + " fora dos lotes continua poison (sem residuo)");

        // GPU x CPU, lote a lote como multiconjunto, e o residuo byte a byte. A referencia CPU recebe
        // a MESMA semente, entao o que ficou intocado tambem tem de coincidir.
        std::vector<FGpuIndirectDrawArgs> ArgsCpu(Capacidade, ComandoPoison());
        std::vector<u32> ContScatterCpu;
        std::vector<u32> CursoresCpu(LotesCpu.size(), 0u);
        ScatterIndirectArgs(In, OffCpu, LotesCpu, 0u, CursoresCpu, ArgsCpu, ContScatterCpu);
        for (u32 B = 0; B < In.BatchCapacity; ++B) {
            if (LotesCpu[B] == 0u) continue;
            const auto A = Ordenados(&G.Args[OffCpu[B]], LotesCpu[B]);
            const auto C = Ordenados(&ArgsCpu[OffCpu[B]], LotesCpu[B]);
            bool Iguais = A.size() == C.size();
            for (size_t K = 0; Iguais && K < A.size(); ++K) Iguais = MesmoComando(A[K], C[K]);
            Check(Iguais, "lote " + std::to_string(B) + ": multiconjunto GPU == CPU");
        }
        for (u32 P = TotalCmd; P < Capacidade; ++P)
            Check(MesmoComando(G.Args[P], ArgsCpu[P]),
                  "residuo na posicao " + std::to_string(P) + ": GPU == CPU");

        // ---- estouro do prefixo: o scatter se ABSTEM por inteiro ----
        //
        // Com os offsets ENVENENADOS, como o prefixo os publica sob estouro. Nao e redundancia com
        // passar offsets bons: e o caso do perigo real. Sem a abstencao, o cursor 1 do lote 0 cai em
        // 0xFFFFFFFF + 1 = 0 e escreve em cima da primeira posicao — que aqui tem de continuar poison.
        const std::vector<u32> OffPoison(OffCpu.size(), kGpuBatchOffsetPoison);
        const FSonda::FScatter E = S.ExecutarScatter(In, OffPoison, LotesCpu, {}, 1u, Capacidade);
        CheckEq(E.Contadores[GpuScatterCounterSuppressed], static_cast<u32>(Slot.size()),
                "sob estouro, todo candidato e suprimido");
        CheckEq(E.Contadores[GpuScatterCounterEmitted], 0u, "e nenhum comando e emitido");
        CheckEq(E.Contadores[GpuScatterCounterFaultCursor] + E.Contadores[GpuScatterCounterFaultCapacity], 0u,
                "nem registrado como falha: a abstencao vem antes de qualquer cursor");
        for (u32 P = 0; P < Capacidade; ++P)
            Check(MesmoComando(E.Args[P], ComandoPoison()),
                  "sob estouro a posicao " + std::to_string(P) + " continua poison");
    }

    // ---- a CADEIA sobre recursos PERSISTENTES: reset -> classify -> prefixo -> scatter -> final ----
    //
    // Os executores acima criam buffers novos a cada chamada e por isso nao enxergam um acumulador que
    // atravessa de uma execucao para a seguinte. Aqui a cadeia roda DUAS vezes sobre os mesmos
    // recursos, como o frame faria, e depois uma terceira sem o reset.
    {
        const u32 Capacidade = static_cast<u32>(Slot.size());
        FCadeia C(S, In, Capacidade);
        C.Criar();

        const FCadeia::FSaida R1 = C.Rodar(FCadeia::kResetTudo);
        const FCadeia::FSaida R2 = C.Rodar(FCadeia::kResetTudo);

        const u32 QueridoLote[8]{ 2u, 1u, 0u, 0u, 0u, 0u, 0u, 0u };
        const FGpuIndirectDrawArgs L0[2]{ Esperado(0u, 0u), Esperado(2u, 0u) };
        const auto Want0 = Ordenados(L0, 2u);
        for (u32 Rodada = 1; Rodada <= 2u; ++Rodada) {
            const FCadeia::FSaida& R = Rodada == 1u ? R1 : R2;
            const std::string N = std::to_string(Rodada) + "a execucao";
            CheckEq(R.Status, 1u, N + ": o frame fecha");
            CheckEq(R.Reason, 0u, N + ": sem motivo de invalidacao");
            for (u32 B = 0; B < In.BatchCapacity; ++B) {
                CheckEq(R.Draw[B], QueridoLote[B], N + ": DrawCounts do lote " + std::to_string(B));
                CheckEq(R.Cursors[B], R.Counts[B], N + ": cursor == contagem no lote " + std::to_string(B));
            }
            CheckEq(R.ScatCont[GpuScatterCounterEmitted], 3u, N + ": tres comandos emitidos");
            const auto Got0 = Ordenados(&R.Args[0], 2u);
            Check(MesmoComando(Got0[0], Want0[0]) && MesmoComando(Got0[1], Want0[1]),
                  N + ": lote 0, multiconjunto exato");
            Check(MesmoComando(R.Args[2], Esperado(1u, 1u)), N + ": lote 1, comando exato");
            for (u32 P = 3; P < Capacidade; ++P)
                Check(MesmoComando(R.Args[P], ComandoPoison()),
                      N + ": posicao " + std::to_string(P) + " continua poison");
        }

        // As duas execucoes com reset produzem o MESMO resultado. E isto que prova que nada atravessou
        // de uma para a outra — um cursor ou uma contagem acumulada apareceria na segunda.
        for (u32 B = 0; B < In.BatchCapacity; ++B)
            CheckEq(R2.Draw[B], R1.Draw[B], "2a == 1a no DrawCounts do lote " + std::to_string(B));

        // ---- a REDE DE SEGURANCA: terceira execucao SEM reset ----
        //
        // Sem reset os quatro acumuladores crescem EM SINCRONIA: as contagens dobram para {4, 2}, os
        // cursores continuam de {2, 1} e terminam em {4, 2}, os emitidos vao a 6. Toda igualdade ENTRE
        // acumuladores continua valendo. So a ANCORA — candidatos desta execucao, vindos do cbuffer —
        // enxerga que os contadores do classify somam 28, e nao 14.
        const FCadeia::FSaida R3 = C.Rodar(0u);
        CheckEq(R3.Counts[0], 4u, "sem reset BatchCounts acumula: 2 + 2");
        CheckEq(R3.Cursors[0], R3.Counts[0],
                "e os cursores acumulam JUNTO — por isso cursor == contagem nao basta");
        CheckEq(R3.Status, 0u, "sem reset o frame e invalidado");
        CheckEq(R3.Reason, static_cast<u32>(GpuFinalizeClassifyAnchor),
                "e o UNICO motivo e a ancora: sem ela, esquecer o reset inteiro passaria");
        for (u32 B = 0; B < In.BatchCapacity; ++B)
            CheckEq(R3.Draw[B], 0u, "sem reset o count buffer sai zerado no lote " + std::to_string(B));

        // A referencia CPU da finalizacao, sobre os MESMOS numeros lidos da GPU, chega ao mesmo veredito.
        std::vector<u32> DrawCpu; u32 MotivoCpu = 0u;
        const u32 StatusCpu = FinalizeDrawCounts(R3.Counts, R3.PrefCont, R3.ScatCont, R3.Cursors,
                                                 R3.ClassCont, Capacidade, DrawCpu, MotivoCpu);
        CheckEq(StatusCpu, R3.Status, "finalizacao CPU == GPU no status");
        CheckEq(MotivoCpu, R3.Reason, "finalizacao CPU == GPU no motivo");

        // ---- cada elo da finalizacao, pela SUA causa: um acumulador sem reset de cada vez ----
        //
        // Antes de cada cenario, uma execucao limpa — que tambem prova que o reset completo recupera
        // de qualquer estado parcial. Os motivos esperados foram calculados a mao a partir do estado
        // que a execucao limpa deixa (contagens {2,1}, cursores {2,1}, 3 emitidos, 14 candidatos):
        //
        //   sem reset dos CURSORES   : comecam em {2,1}, todos os tres passam da contagem
        //                              -> falha de cursor + cursor != contagem
        //   sem reset das CONTAGENS  : viram {4,2} contra 3 indiretos frescos
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
            FinalizeDrawCounts(R.Counts, R.PrefCont, R.ScatCont, R.Cursors, R.ClassCont, Capacidade,
                               DCpu, MCpu);
            CheckEq(MCpu, R.Reason, N + ": finalizacao CPU == GPU no motivo");
        }
    }

    // O prefixo com `BatchCounts` SEMEADO, fora do fluxo encadeado.
    //
    // Motivo: pelo fluxo normal as contagens vem do classify e a soma nunca pode estourar, entao a
    // protecao de estouro do HLSL nunca e executada — quebra-la continuaria verde. Aqui ela e
    // alimentada direto.
    {
        // Caso normal, mesmo vetor do teste CPU. O oraculo continua escrito a mao.
        const std::vector<u32> Contagens{ 2u, 0u, 5u, 1u, 0u, 3u };
        const u32 QueridoOff[6]{ 0u, 2u, 2u, 7u, 8u, 8u };
        const FSonda::FPrefixo P = S.ExecutarPrefixo(Contagens);
        for (u32 I = 0; I < 6u; ++I)
            CheckEq(P.Offsets[I], QueridoOff[I], "prefixo isolado, offset " + std::to_string(I));
        CheckEq(P.Contadores[GpuPrefixCounterTotal], 11u, "prefixo isolado: total");
        CheckEq(P.Contadores[GpuPrefixCounterOverflow], 0u, "prefixo isolado: sem estouro");

        // E a referencia CPU sobre as mesmas contagens.
        std::vector<u32> OffCpu; u32 EstourouCpu = 0u;
        PrefixBatchCounts(Contagens, OffCpu, EstourouCpu);
        for (u32 I = 0; I < 6u; ++I)
            CheckEq(OffCpu[I], P.Offsets[I], "prefixo isolado, CPU x GPU em " + std::to_string(I));
    }
    {
        // O ESTOURO, que o fluxo encadeado nao alcanca. A soma passa de u32 no indice 1.
        const std::vector<u32> Contagens{ 0xFFFFFFF0u, 0x20u, 1u, 7u };
        const FSonda::FPrefixo P = S.ExecutarPrefixo(Contagens);
        CheckEq(P.Contadores[GpuPrefixCounterOverflow], 1u,
                "o HLSL marca a soma que daria a volta");
        CheckEq(P.Offsets[0], 0u, "a entrada anterior ao estouro continua correta");
        for (u32 I = 1; I < 4u; ++I)
            CheckEq(P.Offsets[I], kGpuBatchOffsetPoison,
                    "da entrada que falhou em diante, poison (" + std::to_string(I) + ")");

        // Elemento a elemento contra a referencia CPU, inclusive o poison: e o que prova que os dois
        // lados implementam o MESMO contrato pos-estouro, e nao dois contratos parecidos.
        std::vector<u32> OffCpu; u32 EstourouCpu = 0u;
        PrefixBatchCounts(Contagens, OffCpu, EstourouCpu);
        CheckEq(EstourouCpu, P.Contadores[GpuPrefixCounterOverflow], "os dois marcam estouro");
        for (u32 I = 0; I < 4u; ++I)
            CheckEq(OffCpu[I], P.Offsets[I], "pos-estouro, CPU x GPU em " + std::to_string(I));
    }
    // O prefixo, sobre o `BatchCounts` que o classify acabou de escrever — sem readback entre os
    // dois, como sera na producao. O oraculo continua escrito a mao.
    {
        const u32 QueridoOffset[8]{ 0u, 2u, 3u, 3u, 3u, 3u, 3u, 3u };
        for (u32 B = 0; B < In.BatchCapacity; ++B)
            CheckEq(Gpu.Offsets[B], QueridoOffset[B],
                    "offset do lote " + std::to_string(B) + " na GPU");
        CheckEq(Gpu.PrefixCont[GpuPrefixCounterTotal], 3u, "o total do prefixo e o de comandos");
        CheckEq(Gpu.PrefixCont[GpuPrefixCounterOverflow], 0u, "e o prefixo nao estourou");

        // E a referencia CPU sobre as MESMAS contagens.
        std::vector<u32> OffCpu; u32 EstourouCpu = 0u;
        const u32 TotalCpu = PrefixBatchCounts(LotesCpu, OffCpu, EstourouCpu);
        CheckEq(TotalCpu, Gpu.PrefixCont[GpuPrefixCounterTotal], "GPU e CPU somam o mesmo total");
        for (u32 B = 0; B < In.BatchCapacity; ++B)
            CheckEq(OffCpu[B], Gpu.Offsets[B], "offset do lote " + std::to_string(B) + ": CPU x GPU");

        // A propriedade que o scatter vai depender: o intervalo de cada lote nao invade o do
        // seguinte. `Offset[b] + Count[b] <= Offset[b+1]`, com igualdade porque o prefixo e denso.
        for (u32 B = 0; B + 1 < In.BatchCapacity; ++B)
            CheckEq(Gpu.Offsets[B] + Gpu.Lotes[B], Gpu.Offsets[B + 1],
                    "o lote " + std::to_string(B) + " termina onde o seguinte comeca");
        CheckEq(Gpu.Offsets[In.BatchCapacity - 1u] + Gpu.Lotes[In.BatchCapacity - 1u],
                Gpu.PrefixCont[GpuPrefixCounterTotal], "e o ultimo termina no total");
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
        for (const u32 C : G.Contadores) Soma += C;
        CheckEq(Soma, static_cast<u32>(Slot.size()), "overflow tambem e UM desfecho, nao zero");
    }

    // Lista vazia: nenhum dispatch, nenhum contador. Sem este caso, um shader que lesse
    // `ExpandedSlots[0]` fora de guarda so apareceria numa cena sem candidatos.
    {
        FGpuClassifyInput Vazia = In;
        Vazia.Slots = {};
        const FResultado G = S.Executar(Vazia);
        u32 Soma = 0;
        for (const u32 C : G.Contadores) Soma += C;
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
