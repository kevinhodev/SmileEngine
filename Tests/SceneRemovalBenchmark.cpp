// Custo de uma remocao estrutural na FScene, em funcao de ONDE o objeto esta na lista.
//
// Por que existe: a Fase 1 do GPU-DRIVEN-PLAN mantem o `erase` ESTAVEL de proposito — as pastas
// do Scene Outliner sao ranges [begin,end) sobre a lista, e a visibilidade persistida enderecca
// por offset, entao swap-and-pop trocaria uma mesh de asset para dentro de pasta alheia. O preco
// declarado dessa escolha e `erase + RebuildRenderableIndex()` continuar O(N) por edicao.
//
// "O(N) por edicao de editor e irrelevante" e uma afirmacao sobre MAGNITUDE, e magnitude nao se
// argumenta: se mede. Se remover no inicio da cena de 1M custar centenas de milissegundos, a
// escolha continua certa para o caminho por frame e passa a ser um problema de INTERACAO — e isso
// muda a prioridade de migrar o Outliner para membership por ObjectId, que hoje esta
// deliberadamente fora do escopo.
//
// CPU pura, como o SceneIdentityTests: sem device, sem malha de verdade (a FScene guarda o
// FGpuMesh* e nunca o segue).

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "Smile/Scene/Scene.h"

namespace {
    using Clock = std::chrono::steady_clock;

    double Milliseconds(Clock::duration D) {
        return std::chrono::duration<double, std::milli>(D).count();
    }

    Smile::FGpuMesh* FakeMesh(std::uintptr_t Tag) {
        return reinterpret_cast<Smile::FGpuMesh*>(Tag);
    }

    // Nome com tamanho realista: a FRenderable carrega std::string, e um nome curto o suficiente
    // para caber na small-string optimization mediria uma struct que a cena real nao tem.
    Smile::FScene BuildScene(Smile::u32 Count) {
        Smile::FScene Scene;
        Smile::FRenderable Template;
        Template.Mesh = FakeMesh(0x1000);
        for (Smile::u32 i = 0; i < Count; ++i) {
            Template.Name = "renderable_com_nome_de_tamanho_plausivel_" + std::to_string(i);
            Template.CookedIndex = static_cast<Smile::i32>(i);
            Scene.AddRenderable(Template);
        }
        return Scene;
    }
}

int main(int argc, char** argv) {
    Smile::u32 Count = 1'000'000;
    for (int A = 1; A < argc; ++A) {
        if (std::string(argv[A]) == "--count" && A + 1 < argc) {
            // Parse COMPLETO. `strtoul` sozinho aceitava zero e texto invalido como zero, e dai
            // `Lista.size() - 1` fazia underflow e o acesso caia em 0xC0000005 — o benchmark
            // morria com violacao de acesso em vez de reclamar do argumento.
            const std::string Bruto = argv[++A];
            errno = 0;
            char* Fim = nullptr;
            const unsigned long long Valor = std::strtoull(Bruto.c_str(), &Fim, 10);
            if (Bruto.empty() || Bruto.find('-') != std::string::npos || Fim == Bruto.c_str() ||
                *Fim != '\0' || errno == ERANGE || Valor == 0 ||
                Valor > std::numeric_limits<Smile::u32>::max()) {
                std::fprintf(stderr,
                             "--count invalido: '%s' (esperado inteiro em [1, %u])\n",
                             Bruto.c_str(), std::numeric_limits<Smile::u32>::max());
                return 2;
            }
            Count = static_cast<Smile::u32>(Valor);
            continue;
        }
        std::fprintf(stderr, "argumento desconhecido: %s\n", argv[A]);
        return 2;
    }

    std::printf("[bench] montando %u renderaveis...\n", Count);
    const auto BuildStart = Clock::now();
    Smile::FScene Scene = BuildScene(Count);
    std::printf("[bench] montagem: %.1f ms  (%zu na lista, %zu B por FRenderable)\n",
                Milliseconds(Clock::now() - BuildStart), Scene.Renderables().size(),
                sizeof(Smile::FRenderable));

    // Uma remocao por posicao, cada uma numa cena recem-montada: remover tres vezes seguidas
    // encolheria a lista e a segunda medicao ja nao seria de N.
    struct FCaso { const char* Rotulo; double Fracao; };
    const FCaso Casos[] = { { "inicio", 0.0 }, { "meio", 0.5 }, { "fim", 1.0 } };

    for (const FCaso& Caso : Casos) {
        Smile::FScene Local = BuildScene(Count);
        // Tamanho por VALOR, e nao por referencia. `Renderables()` devolve referencia ao mesmo
        // vetor, entao comparar o size() depois com o size() de uma referencia guardada antes era
        // comparar o vetor consigo mesmo: a condicao nunca poderia detectar uma implementacao que
        // devolvesse true sem remover nada.
        const size_t TamanhoAntes = Local.Renderables().size();
        const size_t Posicao = Caso.Fracao >= 1.0
            ? TamanhoAntes - 1
            : static_cast<size_t>(static_cast<double>(TamanhoAntes) * Caso.Fracao);
        const Smile::u64 Id = Local.Renderables()[Posicao].Id;

        const auto Start = Clock::now();
        const bool Removido = Local.RemoveRenderable(Id);
        const double Ms = Milliseconds(Clock::now() - Start);

        std::printf("[bench] remocao no %-7s (indice %9zu): %8.2f ms  %s\n",
                    Caso.Rotulo, Posicao, Ms, Removido ? "" : "FALHOU");
        if (!Removido) {
            std::fprintf(stderr, "[bench] RemoveRenderable devolveu false\n");
            return 1;
        }
        if (Local.Renderables().size() != TamanhoAntes - 1) {
            std::fprintf(stderr, "[bench] a lista tem %zu, esperado %zu\n",
                         Local.Renderables().size(), TamanhoAntes - 1);
            return 1;
        }
        // E o Id removido nao pode mais resolver: o encolhimento sozinho nao prova que saiu o
        // objeto CERTO, nem que o mapa Id->indice foi atualizado.
        if (Local.IndexOfRenderable(Id) >= 0) {
            std::fprintf(stderr, "[bench] o Id removido continua resolvendo\n");
            return 1;
        }
    }

    std::printf("[bench] ok\n");
    return 0;
}
