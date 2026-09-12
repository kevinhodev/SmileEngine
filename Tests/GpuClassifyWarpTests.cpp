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
            std::vector<u32> Draw;
            const u32 Total = PrefixBatchCounts(Contagens, Offsets, Draw, Estourou);
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
            std::vector<u32> Draw{ 9u };
            CheckEq(PrefixBatchCounts({}, Offsets, Draw, Estourou), 0u, "lista vazia soma zero");
            CheckEq(Estourou, 0u, "e nao marca estouro");
            Check(Offsets.empty(), "e a saida fica vazia, e nao com o conteudo anterior");
        }
        {
            // A volta em u32. Inalcancavel a partir do classify — cada candidato contribui no
            // maximo 1 —, mas o shader nao sabe quem escreveu o buffer que ele soma. Um prefixo que
            // da a volta aponta comandos para dentro dos comandos de outro lote.
            const u32 Contagens[3]{ 0xFFFFFFF0u, 0x20u, 1u };
            std::vector<u32> Offsets; u32 Estourou = 0u;
            std::vector<u32> Draw;
            PrefixBatchCounts(Contagens, Offsets, Draw, Estourou);
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
            FGpuBatchKey Key{};
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

        FGpuBatchKey K{};
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
            FGpuBatchKey Key{};
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
}

namespace {
    struct FResultado {
        std::vector<u32> Lotes;
        std::vector<u32> Contadores;
        // Saida do prefixo, produzida no MESMO command list, a partir do BatchCounts que o classify
        // acabou de escrever. Encadear na GPU e o ponto: na producao nao ha readback entre os dois.
        std::vector<u32> Offsets;
        std::vector<u32> PrefixCont;
        std::vector<u32> DrawCounts;
    };

    // A sonda. Mesma forma da do expand; o que muda sao as views e o CB.
    struct FSonda {
        wrl::ComPtr<ID3D12Device>        Device;
        wrl::ComPtr<ID3D12CommandQueue>  Queue;
        wrl::ComPtr<ID3D12RootSignature> Root;
        wrl::ComPtr<ID3D12RootSignature> RootPrefixo;
        wrl::ComPtr<ID3D12PipelineState> Pipeline;
        wrl::ComPtr<ID3D12PipelineState> PipelinePrefixo;
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

        void Construir(const std::vector<u8>& _Bytecode, const std::vector<u8>& _Prefixo) {
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

            // O prefixo tem root signature PROPRIA: uma SRV e TRES UAVs (offsets, contadores e o
            // count buffer sanitizado). Reaproveitar a do classify
            // deixaria tres SRVs do range ligados sem descritor inicializado, que e coisa que a
            // camada de debug reclama e que nao tem nada a ver com o que esta sob teste.
            D3D12_DESCRIPTOR_RANGE RangesP[2]{};
            RangesP[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            RangesP[0].NumDescriptors = 1;
            RangesP[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            RangesP[1].NumDescriptors = 3; // BatchOffsets, Counters, DrawCounts
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

            D3D12_COMMAND_QUEUE_DESC QD{};
            Hr(Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&Queue)), "CreateCommandQueue");
            Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence");
            Step = Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            Drenar("criacao");
        }


        struct FPrefixo {
            std::vector<u32> DrawCounts;
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
            auto BufDraw = Buffer(D3D12_HEAP_TYPE_DEFAULT, OffBytes, true,
                                  D3D12_RESOURCE_STATE_COMMON);

            const std::vector<u32> Semente(static_cast<size_t>(OffBytes / 4), 0xA5A5A5A5u);
            const u32 ZerosCont[GpuPrefixCounterCount]{};
            auto BufSemente = Upload(Semente.data(), OffBytes);
            auto BufZeros   = Upload(ZerosCont, ContBytes);

            auto LeituraOff  = Buffer(D3D12_HEAP_TYPE_READBACK, OffBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraCont = Buffer(D3D12_HEAP_TYPE_READBACK, ContBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraDraw = Buffer(D3D12_HEAP_TYPE_READBACK, OffBytes, false,
                                      D3D12_RESOURCE_STATE_COPY_DEST);

            wrl::ComPtr<ID3D12DescriptorHeap> Heap;
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = 4;
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
            UAV(BufDraw.Get(), Slots, 3);

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
            // O DrawCounts tambem nasce com o padrao reconhecivel: zerado, nao daria para separar
            // "o shader escreveu zero" de "o shader nao escreveu".
            List->CopyBufferRegion(BufDraw.Get(), 0, BufSemente.Get(), 0, OffBytes);
            Transicao(BufOffsets.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufDraw.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
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
            Transicao(BufDraw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraOff.Get(), BufOffsets.Get());
            List->CopyResource(LeituraCont.Get(), BufCont.Get());
            List->CopyResource(LeituraDraw.Get(), BufDraw.Get());
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
            Saida.DrawCounts.resize(Slots);
            if (Slots > 0) {
                const u32* P = nullptr;
                Hr(LeituraDraw->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u32**>(&P))),
                   "Map draw counts isolados");
                std::memcpy(Saida.DrawCounts.data(), P, Slots * sizeof(u32));
                LeituraDraw->Unmap(0, nullptr);
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
            auto BufDrawCounts = Buffer(D3D12_HEAP_TYPE_DEFAULT, LoteBytes, true,
                                        D3D12_RESOURCE_STATE_COMMON);
            const u32 ZerosPref[GpuPrefixCounterCount]{};
            auto BufZerosPref = Upload(ZerosPref, PrefContBytes);
            auto LeituraOffsets  = Buffer(D3D12_HEAP_TYPE_READBACK, LoteBytes, false,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraPrefCont = Buffer(D3D12_HEAP_TYPE_READBACK, PrefContBytes, false,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraDrawCounts = Buffer(D3D12_HEAP_TYPE_READBACK, LoteBytes, false,
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
            UAV(BufDrawCounts.Get(), _In.BatchCapacity, 9);

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
            List->CopyBufferRegion(BufDrawCounts.Get(), 0, BufZerosLote.Get(), 0, LoteBytes);
            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufOffsets.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufPrefCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transicao(BufDrawCounts.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
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
            Transicao(BufDrawCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraOffsets.Get(), BufOffsets.Get());
            List->CopyResource(LeituraPrefCont.Get(), BufPrefCont.Get());
            List->CopyResource(LeituraDrawCounts.Get(), BufDrawCounts.Get());

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
            Saida.DrawCounts.resize(_In.BatchCapacity);
            if (_In.BatchCapacity > 0) {
                const u32* P = nullptr;
                Hr(LeituraDrawCounts->Map(0, nullptr,
                                          reinterpret_cast<void**>(const_cast<u32**>(&P))),
                   "Map draw counts");
                std::memcpy(Saida.DrawCounts.data(), P, _In.BatchCapacity * sizeof(u32));
                LeituraDrawCounts->Unmap(0, nullptr);
            }
            return Saida;
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

    FSonda S;
    S.Construir(Bytecode, BytecodePrefixo);

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
        std::vector<u32> DrawCpu;
        PrefixBatchCounts(Contagens, OffCpu, DrawCpu, EstourouCpu);
        for (u32 I = 0; I < 6u; ++I)
            CheckEq(OffCpu[I], P.Offsets[I], "prefixo isolado, CPU x GPU em " + std::to_string(I));
        // O COUNT BUFFER sanitizado. Sem estouro, ele e copia das contagens.
        for (u32 I = 0; I < 6u; ++I)
            CheckEq(P.DrawCounts[I], Contagens[I],
                    "sem estouro, DrawCounts copia BatchCounts em " + std::to_string(I));
        for (u32 I = 0; I < 6u; ++I)
            CheckEq(DrawCpu[I], P.DrawCounts[I], "DrawCounts CPU x GPU em " + std::to_string(I));
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
        std::vector<u32> DrawCpu;
        PrefixBatchCounts(Contagens, OffCpu, DrawCpu, EstourouCpu);
        CheckEq(EstourouCpu, P.Contadores[GpuPrefixCounterOverflow], "os dois marcam estouro");
        for (u32 I = 0; I < 4u; ++I)
            CheckEq(OffCpu[I], P.Offsets[I], "pos-estouro, CPU x GPU em " + std::to_string(I));

        // ⚠️ O ponto do achado: um `return` no scatter NAO suprime draw nenhum, porque o
        // `ExecuteIndirect` le o count buffer direto e executaria as contagens antigas sobre
        // argumentos que o scatter deixou intactos. Sob estouro, TODAS as entradas do count buffer
        // tem de sair zeradas — e zeradas de verdade, escritas pelo shader: o buffer nasce com
        // 0xA5A5A5A5 justamente para que "nao escreveu" nao passe por "escreveu zero".
        for (u32 I = 0; I < 4u; ++I)
            CheckEq(P.DrawCounts[I], 0u,
                    "sob estouro o count buffer sai ZERADO em " + std::to_string(I));
        for (u32 I = 0; I < 4u; ++I)
            CheckEq(DrawCpu[I], P.DrawCounts[I],
                    "count buffer pos-estouro, CPU x GPU em " + std::to_string(I));
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
        std::vector<u32> DrawCpu2;
        const u32 TotalCpu = PrefixBatchCounts(LotesCpu, OffCpu, DrawCpu2, EstourouCpu);
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
