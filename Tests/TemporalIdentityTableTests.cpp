// Tabela de identidades por frame e remap de slot (Smile/Scene/CapturedIdentity.h).
//
// O que este arquivo cobre e o PREENCHIMENTO, que e codigo de producao compartilhado com o
// FTemporalMotionVectors: se a tabela ou o remap sairem errados, a cadeia erra em qualquer
// consumidor, shader nenhum salva. As funcoes sao templadas no acessor exatamente para que o teste
// execute AS MESMAS, e nao uma copia parecida.
//
// A resolucao em si roda no HLSL e nao pode ser executada aqui -- exigiria GPU, e esta suite roda
// no CI sem uma. O que prende o shader a esta especificacao e o gate de FONTE no fim do arquivo, o
// mesmo criterio ja usado para o decoder do id.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Smile/Graphics/PostProcess/TemporalMotionVectors.h"
#include "Smile/Scene/CapturedIdentity.h"
#include "Smile/Scene/GpuSceneHandle.h"
#include "Smile/Scene/Scene.h"

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    using namespace Smile;

    constexpr FGpuFenceSet kNenhuma{};
    constexpr FGpuFenceSet kTodas{ 1000, 1000 };

    // A caminhada da cadeia, escrita AQUI de proposito e nao como utilitario reutilizavel: ela nao
    // e a implementacao de producao (a de producao e o HLSL), e sim a especificacao contra a qual o
    // gate de fonte compara. Publica-la como helper compartilhado daria a impressao errada de que
    // algum consumidor a executa.
    bool Resolve(const std::vector<FGpuIdentityEntry>& Anterior,
                 const std::vector<u32>&               Remap,
                 const std::vector<FGpuIdentityEntry>& Atual,
                 u32 IdAnterior, u32 ContagemAtual, u32& IndiceAtual) {
        IndiceAtual = 0;
        if (IdAnterior >= Anterior.size()) return false;

        const FGpuIdentityEntry Era = Anterior[IdAnterior];
        if (Era.Generation == 0 || Era.Slot >= Remap.size()) return false;

        const u32 Candidato = Remap[Era.Slot];
        if (Candidato >= ContagemAtual) return false;

        const FGpuIdentityEntry Agora = Atual[Candidato];
        if (Agora.Slot != Era.Slot || Agora.Generation != Era.Generation) return false;

        IndiceAtual = Candidato;
        return true;
    }

    // Os buffers chegam SUJOS de proposito.
    //
    // O destino real e um staging reusado a cada frame: ele contem o frame anterior quando o
    // preenchimento comeca, e e exatamente por isso que a limpeza existe. Entregar um vetor
    // recem-construido (que o std::vector ja zera) faria a assercao "o resto fica zerado" passar
    // sozinha, sem a producao limpar nada -- foi o que aconteceu na primeira versao deste teste.
    std::vector<FGpuIdentityEntry> Tabela(const std::vector<FGpuSceneHandle>& Lista, u32 Capacidade) {
        // Generation != 0 de proposito: sujeira que PARECE objeto vivo.
        std::vector<FGpuIdentityEntry> Out(Capacidade, FGpuIdentityEntry{ 0xBADF00Du, 0xBADF00Du });
        BuildIdentityTable(Out.data(), Capacidade, static_cast<u32>(Lista.size()),
                           [&](u32 i) { return Lista[i]; });
        return Out;
    }

    std::vector<u32> RemapDe(const std::vector<FGpuSceneHandle>& Lista, u32 CapacidadeSlots) {
        // Sujeira que parece indice valido, pelo mesmo motivo.
        std::vector<u32> Out(CapacidadeSlots, 0u);
        BuildSlotRemap(Out.data(), CapacidadeSlots, static_cast<u32>(Lista.size()),
                       [&](u32 i) { return Lista[i]; });
        return Out;
    }

    std::string LerArquivo(const char* Caminho, bool& Abriu) {
        std::ifstream Arquivo(Caminho);
        Abriu = Arquivo.is_open();
        if (!Abriu) return {};
        std::stringstream Buffer;
        Buffer << Arquivo.rdbuf();
        return Buffer.str();
    }
}

int main() {
    constexpr u32 kCapacidade = 8;
    constexpr u32 kSlots      = 8;

    // 1. A TABELA GUARDA A RESIDENCIA DE CADA POSICAO, E ZERA O RESTO.
    //
    // O resto zerado nao e detalhe: generation 0 nunca e alocada, entao e ela que o leitor usa
    // para separar "posicao vazia" de "objeto real" -- sem isso, lixo de um frame anterior
    // resolveria como objeto valido.
    {
        FGpuSceneSlots Slots;
        const FGpuSceneHandle A = Slots.Allocate();
        const FGpuSceneHandle B = Slots.Allocate();

        const auto T = Tabela({ A, B }, kCapacidade);
        Check(T[0].Slot == A.Slot && T[0].Generation == A.Generation,
              "a posicao 0 guarda a residencia do primeiro objeto");
        Check(T[1].Slot == B.Slot && T[1].Generation == B.Generation,
              "a posicao 1 guarda a do segundo");
        Check(A.Generation != 0 && B.Generation != 0,
              "o allocator nunca entrega generation 0 -- e o que faz o zero significar vazio");
        for (u32 i = 2; i < kCapacidade; ++i) {
            Check(T[i].Generation == 0, "posicao alem da contagem viva fica zerada");
        }
    }

    // 2. O REMAP APONTA CADA SLOT PARA O INDICE ATUAL, E MARCA O RESTO COMO INVALIDO.
    {
        FGpuSceneSlots Slots;
        const FGpuSceneHandle A = Slots.Allocate();
        const FGpuSceneHandle B = Slots.Allocate();
        const FGpuSceneHandle C = Slots.Allocate();

        const auto R = RemapDe({ C, A }, kSlots); // ordem trocada de proposito
        Check(R[C.Slot] == 0, "o remap leva ao indice na lista, e nao a ordem de alocacao");
        Check(R[A.Slot] == 1, "e o segundo da lista aponta para 1");
        Check(R[B.Slot] == FGpuSceneHandle::kInvalidSlot,
              "slot fora da lista de agora fica invalido");
    }

    // 3. [A,B,C] -> [B,C]: O ID ANTIGO RESOLVE PARA O INDICE NOVO DO MESMO OBJETO.
    //
    // Este e o caso que a fase existe para consertar, e a assercao de contraste e o que lhe da
    // sentido: o id 1 designava B, e na lista de agora a posicao 1 e C. Sem a cadeia, o consumidor
    // pegaria o transform de C achando que era B -- sem erro, so movimento errado.
    {
        FGpuSceneSlots Slots;
        const FGpuSceneHandle A = Slots.Allocate();
        const FGpuSceneHandle B = Slots.Allocate();
        const FGpuSceneHandle C = Slots.Allocate();

        const auto Anterior = Tabela({ A, B, C }, kCapacidade);
        const auto Atual    = Tabela({ B, C }, kCapacidade);
        const auto Remap    = RemapDe({ B, C }, kSlots);

        u32 Indice = 0;
        Check(Resolve(Anterior, Remap, Atual, 1u, 2u, Indice),
              "o id anterior de B ainda resolve depois da remocao de A");
        Check(Indice == 0, "e resolve para a posicao NOVA de B, que e 0");

        Check(Atual[1].Slot == C.Slot,
              "a posicao 1 de agora e C: sem a cadeia, o id 1 entregaria o objeto errado");
        Check(B.Slot != C.Slot, "B e C ocupam residencias distintas, entao a troca seria visivel");
    }

    // 4. OBJETO REMOVIDO NAO RESOLVE.
    {
        FGpuSceneSlots Slots;
        const FGpuSceneHandle A = Slots.Allocate();
        const FGpuSceneHandle B = Slots.Allocate();

        const auto Anterior = Tabela({ A, B }, kCapacidade);
        const auto Atual    = Tabela({ B }, kCapacidade);
        const auto Remap    = RemapDe({ B }, kSlots);

        u32 Indice = 0;
        Check(!Resolve(Anterior, Remap, Atual, 0u, 1u, Indice),
              "o id de um objeto removido nao resolve, e cai no fallback");
    }

    // 5. SLOT RECICLADO NAO RESOLVE -- E SO A GENERATION SEPARA.
    //
    // O contraste aqui e o ponto: o slot volta a ser valido e o remap aponta para um indice REAL,
    // entao toda checagem de residencia passa. O unico teste que reprova e a generation. Remove-la
    // do leitor deixa este caso verde com o OBJETO ERRADO.
    {
        FGpuSceneSlots Slots;
        const FGpuSceneHandle A = Slots.Allocate();
        const FGpuSceneHandle B = Slots.Allocate();

        const auto Anterior = Tabela({ A, B }, kCapacidade);

        Slots.Retire(A, kNenhuma);
        Slots.Collect(kTodas);
        const FGpuSceneHandle D = Slots.Allocate();
        Check(D.Slot == A.Slot, "o slot de A foi reciclado para D");
        Check(D.Generation != A.Generation, "e a generation avancou");

        const auto Atual = Tabela({ B, D }, kCapacidade);
        const auto Remap = RemapDe({ B, D }, kSlots);

        Check(Remap[A.Slot] < 2u,
              "o slot de A aponta para um indice VALIDO agora -- a residencia sozinha aprovaria");

        u32 Indice = 0;
        Check(!Resolve(Anterior, Remap, Atual, 0u, 2u, Indice),
              "mas o id de A nao resolve para D: a generation reprova o ocupante novo");
    }

    // 6. SLOT ALEM DA CAPACIDADE DO REMAP E REJEITADO, SEM LER FORA.
    //
    // O pool de slots inclui os APOSENTADOS, entao ele pode passar do buffer sem que a cena tenha
    // crescido. O desfecho tem de ser fallback, e nunca leitura fora.
    {
        FGpuSceneSlots Slots;
        std::vector<FGpuSceneHandle> Lista;
        for (u32 i = 0; i < 6; ++i) Lista.push_back(Slots.Allocate());

        const auto Anterior = Tabela(Lista, kCapacidade);
        constexpr u32 kRemapCurto = 3; // menor que os slots em uso
        const auto Remap = RemapDe(Lista, kRemapCurto);

        for (u32 i = 0; i < kRemapCurto; ++i) {
            Check(Remap[i] != FGpuSceneHandle::kInvalidSlot,
                  "os slots que cabem foram escritos");
        }

        const auto Atual = Tabela(Lista, kCapacidade);
        u32 Indice = 0;
        Check(!Resolve(Anterior, Remap, Atual, 5u, 6u, Indice),
              "o id cujo slot passa da capacidade do remap e rejeitado");
    }

    // 7. CONTAGEM ANTERIOR: UM ID DA LISTA MAIOR NAO PODE PASSAR PELA ATUAL.
    //
    // A tabela anterior tem tantas entradas quanto a capacidade, entao ela sozinha nao reprova um
    // id alto -- quem reprova e validar a superficie ANTERIOR contra a contagem ANTERIOR. Este
    // caso fixa a razao de IdentityParams.x existir.
    {
        FGpuSceneSlots Slots;
        std::vector<FGpuSceneHandle> Grande;
        for (u32 i = 0; i < 5; ++i) Grande.push_back(Slots.Allocate());

        const auto Anterior = Tabela(Grande, kCapacidade);
        const std::vector<FGpuSceneHandle> Pequena{ Grande[0], Grande[1] };
        const auto Atual = Tabela(Pequena, kCapacidade);
        const auto Remap = RemapDe(Pequena, kSlots);

        Check(Anterior[4].Generation != 0,
              "a entrada 4 da tabela anterior existe: a tabela sozinha nao reprova o id");

        u32 Indice = 0;
        Check(!Resolve(Anterior, Remap, Atual, 4u, 2u, Indice),
              "e o objeto que saiu nao resolve, porque o remap ja nao o aponta");
    }

    // 8. CHURN CRESCE O POOL SEM A CONTAGEM VIVA MUDAR, E O REMAP TEM DE ACOMPANHAR.
    //
    // Achado do review. Aposentar e realocar mantendo 8 vivos leva o pool a 88 enquanto o remap,
    // dimensionado no setup, fica em 72 -- e o Collect devolve slots mas NAO renumera os
    // sobreviventes, entao os 8 vivos ficam sem mapeamento e assim continuam. Nenhuma condicao de
    // resize do renderer pegava isso, porque todas olhavam a contagem de renderables.
    {
        FGpuSceneSlots Slots;
        std::vector<FGpuSceneHandle> Vivos;
        for (u32 i = 0; i < 8; ++i) Vivos.push_back(Slots.Allocate());

        auto Churn = [&](u32 Rodadas) {
            static FGpuFenceSet Fence{};
            for (u32 R = 0; R < Rodadas; ++R) {
                for (FGpuSceneHandle& H : Vivos) {
                    ++Fence.Direct;
                    ++Fence.AsyncCompute;
                    Slots.Retire(H, Fence);
                    H = Slots.Allocate();
                }
            }
            Slots.Collect(FGpuFenceSet{ 1000000, 1000000 });
        };

        // FASE 1 -- dimensionar so pela cena esta errado, e o churn prova.
        const u32 SoPelaCena = SceneCapacityFor(static_cast<u32>(Vivos.size()));
        Churn(10);
        Check(Slots.Capacity() > SoPelaCena,
              "o pool passou da capacidade derivada da cena, com a contagem viva parada");

        u32 ForaSoPelaCena = 0;
        for (const FGpuSceneHandle& H : Vivos) {
            if (H.Slot >= SoPelaCena) ++ForaSoPelaCena;
        }
        Check(ForaSoPelaCena > 0,
              "e vivos caem fora dela: o Collect devolve slots, mas nao renumera sobreviventes");

        // FASE 2 -- a regra de producao cobre o POOL, e a folga absorve o churn normal.
        const u32 RemapNoSetup = SlotRemapCapacityFor(SoPelaCena, 8u /*pool no setup*/);
        Check(!SlotRemapNeedsGrowth(Slots.Capacity(), RemapNoSetup),
              "com a folga, esse churn todo ainda cabe: o re-setup nao dispara a cada edicao");
        for (const FGpuSceneHandle& H : Vivos) {
            Check(H.Slot < RemapNoSetup, "e todo vivo continua mapeado");
        }

        // FASE 3 -- churn suficiente estoura ate a folga, e AI o gate tem de disparar.
        Churn(20);
        Check(SlotRemapNeedsGrowth(Slots.Capacity(), RemapNoSetup),
              "passada a folga, a capacidade do POOL dispara o re-setup -- nenhuma condicao "
              "baseada na contagem de renderables veria isso");

        const u32 RemapDepois =
            SlotRemapCapacityFor(SceneCapacityFor(static_cast<u32>(Vivos.size())),
                                 Slots.Capacity());
        for (const FGpuSceneHandle& H : Vivos) {
            Check(H.Slot < RemapDepois, "e apos o re-setup todo vivo volta a mapear");
        }
        Check(RemapDepois > Slots.Capacity(),
              "o remap fica ACIMA do pool: colado nele, o proximo slot estouraria de novo e cada "
              "edicao reconstruiria TLAS e GI");
    }

    // 9. A CAPACIDADE DE SLOTS NAO CABE EM FLOAT.
    //
    // Achado do review. O teto de 2^24 limita o ID de instancia; a capacidade do pool nao herda
    // esse teto, e a folga do SceneCapacityFor ja a leva acima dele. Os dois casos abaixo erram em
    // sentidos OPOSTOS, que e o que torna o defeito dificil de ver.
    {
        static_assert(std::is_same_v<decltype(FTemporalIdentityParams::SlotCapacity), u32>,
                      "a capacidade viaja como inteiro no cbuffer");
        static_assert(std::is_same_v<decltype(FTemporalIdentityParams::PreviousInstanceCount), u32>,
                      "a contagem anterior tambem");

        for (u32 Renderables : { 16000001u, 16000003u }) {
            const u32 Cap = SceneCapacityFor(Renderables);
            Check(Cap > kMaxTemporalInstanceCount,
                  "a capacidade passa do teto de 24 bits, mesmo com a contagem abaixo dele");

            // volatile para o compilador nao dobrar o round-trip: sem isto o MSVC devolve o valor
            // original e a demonstracao mente.
            volatile float ComoFloat = static_cast<float>(Cap);
            Check(static_cast<u32>(ComoFloat) != Cap,
                  "em float ela nao volta igual -- rejeitaria slot valido ou aceitaria indice "
                  "alem do remap");

            const FTemporalIdentityParams P{ 0u, Cap, 0u, 0u };
            Check(P.SlotCapacity == Cap, "em inteiro ela chega exata ao shader");
        }
    }

    // 10. GATE DE FONTE: O SHADER TEM DE FAZER O QUE ESTA ESPECIFICADO ACIMA.
    //
    // Guarda de FONTE, e nao de comportamento -- e o criterio pratico que importa: reverter o
    // leitor para indexar transform com id do frame passado deixa isto vermelho.
    {
        bool Abriu = false;
        const std::string Comum = LerArquivo(SMILE_TEMPORAL_COMMON_PATH, Abriu);
        Check(Abriu, "o header comum dos shaders foi encontrado");
        if (Abriu) {
            const size_t Inicio = Comum.find("bool TemporalResolvePreviousIndex");
            Check(Inicio != std::string::npos, "o resolvedor existe no HLSL");
            if (Inicio != std::string::npos) {
                const size_t Fim = Comum.find("\n}", Inicio);
                const std::string Corpo = Comum.substr(Inicio, Fim - Inicio);

                Check(Corpo.find("was.Generation == 0u") != std::string::npos,
                      "o resolvedor rejeita entrada vazia pela generation");
                Check(Corpo.find("was.Slot >= slotCapacity") != std::string::npos,
                      "e rejeita slot alem da capacidade do remap");
                Check(Corpo.find("candidate >= currentCount") != std::string::npos,
                      "e rejeita indice fora da lista de agora");
                Check(Corpo.find("now.Slot != was.Slot") != std::string::npos &&
                          Corpo.find("now.Generation != was.Generation") != std::string::npos,
                      "e confirma slot E generation contra a tabela atual -- so o slot aceitaria "
                      "o ocupante reciclado");
            }
        }

        // A regra so serve se alguem a CHAMAR. O teste acima pina o predicado; este pedaco
        // pina que o renderer o consulta -- que e a regressao realista aqui, apagar a condicao.
        const std::string Renderer = LerArquivo(SMILE_RENDERERSCENE_PATH, Abriu);
        Check(Abriu, "o RendererScene foi encontrado");
        if (Abriu) {
            Check(Renderer.find("SlotRemapNeedsGrowth") != std::string::npos,
                  "o renderer consulta a capacidade do pool para decidir re-setup");
        }

        const std::string Dual = LerArquivo(SMILE_DUALMOTION_PATH, Abriu);
        Check(Abriu, "o DualMotion foi encontrado");
        if (Abriu) {
            Check(Dual.find("InstanceTransforms[occluderId]") == std::string::npos,
                  "o DualMotion NAO indexa transform com id do frame passado -- era o defeito");
            Check(Dual.find("TemporalResolvePreviousIndex(occluderId") != std::string::npos,
                  "ele resolve o id do oclusor antes de usar");
            Check(Dual.find("IdentityParams.x, occluderId") != std::string::npos,
                  "e valida o oclusor contra a contagem ANTERIOR");
            Check(Dual.find("IdentityParams.x, backgroundId") != std::string::npos,
                  "assim como o fundo, que tambem vem da superficie anterior");
        }
    }

    if (Failures == 0) {
        std::cout << "TemporalIdentityTable: todos os casos passaram\n";
        return 0;
    }
    std::cerr << "TemporalIdentityTable: " << Failures << " falha(s)\n";
    return 1;
}
