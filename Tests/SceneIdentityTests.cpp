// Invariantes da identidade estavel da FScene (F1 do trabalho de cena).
//
// Isto existe porque o INDICE de um renderavel e endereco de GPU em quatro sistemas ao mesmo
// tempo — InstanceID da TLAS, slot de transform do TemporalMotion, entrada do readback do HiZ e
// o ID que o passe de picking grava. Enquanto a cena so crescia no load, o indice podia servir
// de identidade; a partir do momento em que o editor cria e remove objeto, nao pode mais, e um
// erro aqui nao aparece como excecao: aparece como o objeto errado selecionado, motion vector de
// outro objeto ou uma instancia fantasma na TLAS, muitos frames depois.
//
// Tudo abaixo e CPU pura: nao ha device, nao ha mesh de verdade (ponteiros sinteticos servem —
// a FScene nunca os desreferencia), e por isso roda no CI sem GPU.

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Smile/Scene/Scene.h"

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    // A FScene guarda o FGpuMesh* e nunca o segue; qualquer endereco distinto serve de
    // identidade de malha para o que esta sendo testado aqui.
    Smile::FGpuMesh* FakeMesh(std::uintptr_t Tag) {
        return reinterpret_cast<Smile::FGpuMesh*>(Tag);
    }

    Smile::FRenderable Make(const char* Name, std::uintptr_t MeshTag) {
        Smile::FRenderable R;
        R.Name = Name;
        R.Mesh = FakeMesh(MeshTag);
        return R;
    }

    // Toda entrada da lista resolve por Id para a posicao em que de fato esta, e nenhum Id de
    // fora resolve. E a invariante que o mapa Id->indice tem de manter depois de qualquer
    // mutacao — o resto do teste chama isto apos cada passo.
    void CheckIndexIsCoherent(const Smile::FScene& Scene, std::string_view Where) {
        const auto& List = Scene.Renderables();
        for (std::size_t i = 0; i < List.size(); ++i) {
            const std::string At = std::string(Where) + ": indice " + std::to_string(i);
            Check(List[i].Id != 0, At + " tem Id zero");
            Check(Scene.IndexOfRenderable(List[i].Id) == static_cast<int>(i),
                  At + " nao resolve para si mesmo");
            Check(Scene.IdAt(static_cast<Smile::u32>(i)) == List[i].Id,
                  At + ": IdAt discorda da lista");
            Check(Scene.FindRenderable(List[i].Id) == &List[i],
                  At + ": FindRenderable devolve outro objeto");
        }
        Check(Scene.IndexOfRenderable(0) == -1, std::string(Where) + ": Id 0 resolveu");
        Check(Scene.IdAt(static_cast<Smile::u32>(List.size())) == 0,
              std::string(Where) + ": IdAt fora da lista nao devolveu 0");
    }

    void TestIdsSaoUnicosEProprios() {
        Smile::FScene Scene;
        Smile::FRenderable ComIdFalso = Make("A", 1);
        ComIdFalso.Id = 12345; // quem chama nao escolhe identidade, como no AddLight
        const Smile::u64 IdA = Scene.AddRenderable(ComIdFalso).Id;
        const Smile::u64 IdB = Scene.AddRenderable(Make("B", 2)).Id;

        Check(IdA != 0 && IdB != 0, "AddRenderable devolveu Id zero");
        Check(IdA != 12345, "AddRenderable respeitou o Id que veio de fora");
        Check(IdA != IdB, "dois renderaveis nasceram com o mesmo Id");
        CheckIndexIsCoherent(Scene, "apos dois adds");
    }

    // O caso que motiva a fase inteira: remover no MEIO da lista desloca a cauda, e todo Id
    // sobrevivente precisa passar a resolver para a posicao nova.
    void TestRemocaoNoMeioReancoraACauda() {
        Smile::FScene Scene;
        std::vector<Smile::u64> Ids;
        for (int i = 0; i < 6; ++i)
            Ids.push_back(Scene.AddRenderable(Make(("M" + std::to_string(i)).c_str(), i + 1)).Id);

        const Smile::u64 VersaoEstrutura = Scene.StructureVersion();
        const Smile::u64 VersaoTransform = Scene.TransformsVersion();

        Check(Scene.RemoveRenderable(Ids[2]), "remocao de Id valido falhou");

        Check(Scene.Renderables().size() == 5, "a lista nao encolheu");
        Check(Scene.IndexOfRenderable(Ids[2]) == -1, "Id removido ainda resolve");
        Check(Scene.StructureVersion() != VersaoEstrutura, "remocao nao bumpou StructureVersion");
        Check(Scene.TransformsVersion() != VersaoTransform,
              "remocao nao bumpou TransformsVersion (a TLAS tem uma instancia a menos)");
        CheckIndexIsCoherent(Scene, "apos remover do meio");

        // Ordem relativa preservada: as pastas do Scene Outliner sao ranges [begin,end) sobre
        // esta lista, entao swap-and-pop jogaria uma mesh para dentro de outro asset.
        Check(Scene.Renderables()[0].Id == Ids[0] && Scene.Renderables()[1].Id == Ids[1] &&
              Scene.Renderables()[2].Id == Ids[3] && Scene.Renderables()[3].Id == Ids[4] &&
              Scene.Renderables()[4].Id == Ids[5],
              "a remocao nao preservou a ordem dos sobreviventes");

        // Remover o primeiro e o ultimo sao as bordas do memmove.
        Check(Scene.RemoveRenderable(Ids[0]), "remocao do primeiro falhou");
        CheckIndexIsCoherent(Scene, "apos remover o primeiro");
        Check(Scene.RemoveRenderable(Ids[5]), "remocao do ultimo falhou");
        CheckIndexIsCoherent(Scene, "apos remover o ultimo");

        Check(!Scene.RemoveRenderable(Ids[2]), "remover duas vezes o mesmo Id deu certo");
        Check(!Scene.RemoveRenderable(0), "remover o Id 0 deu certo");
        Check(Scene.Renderables().size() == 3, "remocao invalida mexeu na lista");
    }

    void TestDuplicataCompartilhaMalhaESaiNoFim() {
        Smile::FScene Scene;
        const Smile::u64 IdA = Scene.AddRenderable(Make("A", 7)).Id;
        Scene.AddRenderable(Make("B", 8));

        const Smile::u64 VersaoTransform = Scene.TransformsVersion();
        const Smile::FRenderable* Copia = Scene.DuplicateRenderable(IdA);

        Check(Copia != nullptr, "DuplicateRenderable devolveu nulo para Id valido");
        if (Copia) {
            Check(Copia->Id != IdA, "a copia herdou o Id do original");
            // Mesmo FGpuMesh de proposito: o BLAS e cacheado por ponteiro de mesh, entao a
            // copia entra na TLAS sem construir BLAS novo nem gastar VRAM de geometria.
            Check(Copia->Mesh == FakeMesh(7), "a copia nao compartilhou a malha do original");
            Check(Scene.Renderables().back().Id == Copia->Id,
                  "a copia nao foi para o fim da lista (a pasta residual do outliner conta com isso)");
            Check(Copia->Name != Scene.Renderables()[0].Name,
                  "a copia ficou com o nome identico ao do original");
        }
        Check(Scene.TransformsVersion() != VersaoTransform,
              "duplicata nao bumpou TransformsVersion (a TLAS ganhou uma instancia)");
        CheckIndexIsCoherent(Scene, "apos duplicar");

        Check(Scene.DuplicateRenderable(0) == nullptr, "duplicar o Id 0 deu certo");
        Check(Scene.DuplicateRenderable(999999) == nullptr, "duplicar Id inexistente deu certo");
    }

    // Um Id nunca pode ser reusado dentro da sessao: uma referencia velha (selecao do editor,
    // undo futuro) tem de FALHAR em resolver, nunca apontar em silencio para outro objeto.
    void TestClearNaoReciclaIds() {
        Smile::FScene Scene;
        const Smile::u64 IdAntigo = Scene.AddRenderable(Make("A", 1)).Id;
        Scene.AddRenderable(Make("B", 2));

        const Smile::u64 VersaoEstrutura = Scene.StructureVersion();
        Scene.Clear();

        Check(Scene.Renderables().empty(), "Clear nao esvaziou a lista");
        Check(Scene.StructureVersion() != VersaoEstrutura, "Clear nao bumpou StructureVersion");
        Check(Scene.IndexOfRenderable(IdAntigo) == -1, "Id de antes do Clear ainda resolve");

        const Smile::u64 IdNovo = Scene.AddRenderable(Make("C", 3)).Id;
        Check(IdNovo != IdAntigo, "o Id foi reciclado depois do Clear");
        CheckIndexIsCoherent(Scene, "apos Clear e novo add");
    }

    // Mesh e luz saem do MESMO contador. Este teste falha no desenho anterior, de dois
    // contadores independentes: la a primeira mesh e a primeira luz nasciam ambas com Id 1, e
    // um Id sozinho nao identificava objeto nenhum — quem quisesse guardar "este objeto"
    // precisava carregar o par (tipo, indice) junto.
    void TestIdentidadeUnicaEntreMeshELuz() {
        Smile::FScene Scene;
        std::vector<Smile::u64> Todos;

        // Intercalado de proposito: com contadores separados, o primeiro par ja colide.
        for (int i = 0; i < 3; ++i) {
            Todos.push_back(Scene.AddRenderable(Make("M", i + 1)).Id);
            Smile::FLight L;
            L.Name = "L";
            Todos.push_back(Scene.AddLight(L).Id);
        }

        for (std::size_t a = 0; a < Todos.size(); ++a) {
            Check(Todos[a] != 0, "objeto nasceu sem identidade");
            for (std::size_t b = a + 1; b < Todos.size(); ++b)
                Check(Todos[a] != Todos[b],
                      "dois objetos de cena compartilham Id (mesh e luz colidindo?)");
        }

        // O FindObject resolve os dois tipos, e diz QUAL tipo — sem o chamador ter de saber.
        for (std::size_t i = 0; i < Todos.size(); ++i) {
            const Smile::FSceneObjectRef Ref = Scene.FindObject(Todos[i]);
            Check(Ref.IsValid(), "FindObject nao achou um Id que acabou de ser criado");
            const bool EsperaMesh = (i % 2 == 0);
            Check(Ref.IsRenderable() == EsperaMesh, "FindObject devolveu o tipo errado");
            Check(Ref.IsLight() == !EsperaMesh,     "FindObject devolveu o tipo errado");
            if (Ref.IsRenderable())
                Check(Scene.Renderables()[Ref.Index].Id == Todos[i], "indice de mesh errado");
            else
                Check(Scene.Lights()[Ref.Index].Id == Todos[i], "indice de luz errado");
        }

        Check(!Scene.FindObject(0).IsValid(),       "FindObject(0) resolveu");
        Check(!Scene.FindObject(999999).IsValid(),  "FindObject de Id inexistente resolveu");

        // Remover a mesh nao pode fazer o Id dela passar a resolver como luz (nem o contrario).
        Check(Scene.RemoveRenderable(Todos[0]), "remocao falhou");
        Check(!Scene.FindObject(Todos[0]).IsValid(), "Id de mesh removida ainda resolve");
        Check(Scene.FindObject(Todos[1]).IsLight(),  "a luz perdeu a identidade com a remocao");
    }

    // O ciclo que o editor faz de verdade: cria, apaga do meio, duplica, apaga a copia. Cada
    // passo tem de deixar o mapa coerente para o proximo.
    void TestCicloDeEdicao() {
        Smile::FScene Scene;
        std::vector<Smile::u64> Ids;
        for (int i = 0; i < 4; ++i)
            Ids.push_back(Scene.AddRenderable(Make(("M" + std::to_string(i)).c_str(), i + 1)).Id);

        Scene.RemoveRenderable(Ids[1]);
        CheckIndexIsCoherent(Scene, "ciclo: apos remover");

        const Smile::FRenderable* Copia = Scene.DuplicateRenderable(Ids[3]);
        Check(Copia != nullptr, "ciclo: duplicata falhou apos uma remocao");
        CheckIndexIsCoherent(Scene, "ciclo: apos duplicar");

        if (Copia) {
            const Smile::u64 IdCopia = Copia->Id;
            Check(Scene.RemoveRenderable(IdCopia), "ciclo: remover a copia falhou");
            CheckIndexIsCoherent(Scene, "ciclo: apos remover a copia");
            Check(Scene.IndexOfRenderable(Ids[3]) >= 0,
                  "ciclo: remover a copia derrubou o original");
        }
    }
}

int main() {
    std::cout << "Smile.SceneIdentity\n";
    TestIdsSaoUnicosEProprios();
    TestRemocaoNoMeioReancoraACauda();
    TestDuplicataCompartilhaMalhaESaiNoFim();
    TestClearNaoReciclaIds();
    TestIdentidadeUnicaEntreMeshELuz();
    TestCicloDeEdicao();

    if (Failures == 0) {
        std::cout << "  OK\n";
        return 0;
    }
    std::cerr << "  " << Failures << " falha(s)\n";
    return 1;
}
