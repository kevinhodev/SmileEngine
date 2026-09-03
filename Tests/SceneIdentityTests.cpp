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

namespace Smile {
    // Teto do pool injetavel. A borda real fica em 2^32 slots, e alcanca-la de verdade nao e
    // opcao; o que interessa e o COMPORTAMENTO quando Allocate recusa.
    struct FGpuSceneSlotsTestAccess {
        static void SetMaxSlots(FGpuSceneSlots& Slots, u32 Max) { Slots.MaxSlots = Max; }
    };
}

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    // Sem GPU neste teste, entao nada esta em voo e um slot aposentado pode ser reusado
    // imediatamente. E o unico contexto em que {0,0} e a resposta CERTA, e nao um atalho: no
    // renderer o valor vem de Renderer::CurrentFenceSet(), numa fronteira de submissao.
    const Smile::FGpuFenceSet kSemGpu{ 0, 0 };

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

        Check(Scene.RemoveRenderable(Ids[2], kSemGpu), "remocao de Id valido falhou");

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
        Check(Scene.RemoveRenderable(Ids[0], kSemGpu), "remocao do primeiro falhou");
        CheckIndexIsCoherent(Scene, "apos remover o primeiro");
        Check(Scene.RemoveRenderable(Ids[5], kSemGpu), "remocao do ultimo falhou");
        CheckIndexIsCoherent(Scene, "apos remover o ultimo");

        Check(!Scene.RemoveRenderable(Ids[2], kSemGpu), "remover duas vezes o mesmo Id deu certo");
        Check(!Scene.RemoveRenderable(0, kSemGpu), "remover o Id 0 deu certo");
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
        Scene.Clear(kSemGpu);

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
        Check(Scene.RemoveRenderable(Todos[0], kSemGpu), "remocao falhou");
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

        Scene.RemoveRenderable(Ids[1], kSemGpu);
        CheckIndexIsCoherent(Scene, "ciclo: apos remover");

        const Smile::FRenderable* Copia = Scene.DuplicateRenderable(Ids[3]);
        Check(Copia != nullptr, "ciclo: duplicata falhou apos uma remocao");
        CheckIndexIsCoherent(Scene, "ciclo: apos duplicar");

        if (Copia) {
            const Smile::u64 IdCopia = Copia->Id;
            Check(Scene.RemoveRenderable(IdCopia, kSemGpu), "ciclo: remover a copia falhou");
            CheckIndexIsCoherent(Scene, "ciclo: apos remover a copia");
            Check(Scene.IndexOfRenderable(Ids[3]) >= 0,
                  "ciclo: remover a copia derrubou o original");
        }
    }

    // A residencia GPU sobrevive a troca de cena, e o pool nao cresce por causa dela.
    //
    // Este e o par do TestClearNaoReciclaIds para o outro eixo de identidade: la o argumento e
    // que um Id reusado faria uma referencia velha apontar em silencio para outro objeto; aqui e
    // que uma generation reposta faria um handle da cena anterior voltar a casar com o mesmo par
    // {slot, generation}. A FScene guarda o allocator, e o Clear NAO o reseta.
    //
    // O que os testes CPU-puros do allocator nao alcancam e justamente isto: a politica de
    // persistencia mora no DONO, e um pool testado isolado nunca a ve quebrar.
    void TestClearNaoRessuscitaHandleDeGpu() {
        std::cout << "  troca de cena nao ressuscita handle de GPU\n";
        Smile::FScene Scene;
        Smile::FGpuSceneSlots& Slots = Scene.GpuSlots();

        // O handle vem do PROPRIO renderavel: o AddRenderable ja aloca. Alocar de fora aqui
        // gastaria dois slots por objeto e mediria um pool que nao existe.
        Smile::FGpuSceneHandle CenaAntiga[8];
        for (int i = 0; i < 8; ++i) {
            CenaAntiga[i] =
                Scene.AddRenderable(Make(("velho" + std::to_string(i)).c_str(), 0x100 + i)).GpuHandle;
            Check(Slots.IsAlive(CenaAntiga[i]), "o renderavel nasce com residencia viva");
        }
        const Smile::u32 CapacidadeAntes = Slots.Capacity();
        Check(CapacidadeAntes == 8, "oito slots na cena antiga");

        // O renderer e quem aposenta, porque e quem tem as fences. A ordem e
        // RetireAll -> Collect -> alocar; alocar antes do Collect dobraria o pool.
        // O Clear ja aposenta tudo: e a FScene que nao pode largar um renderavel sem aposentar a
        // residencia dele. Chamar RetireAll por fora seria redundante e mascararia essa garantia.
        const Smile::FGpuFenceSet Required{ 77, 12 };
        Scene.Clear(Required);
        Check(Slots.Collect(Required) == 8, "o Collect drenado devolve os oito slots");

        Smile::FGpuSceneHandle CenaNova[8];
        for (int i = 0; i < 8; ++i) {
            CenaNova[i] =
                Scene.AddRenderable(Make(("novo" + std::to_string(i)).c_str(), 0x200 + i)).GpuHandle;
        }

        Check(Slots.Capacity() == CapacidadeAntes,
              "a capacidade fica inalterada: o allocator do dono persiste e reusa");
        for (const Smile::FGpuSceneHandle& H : CenaAntiga)
            Check(!Slots.IsAlive(H), "nenhum handle da cena anterior ressuscita");
        for (const Smile::FGpuSceneHandle& H : CenaNova)
            Check(Slots.IsAlive(H), "os handles da cena nova estao vivos");

        bool TodasMaiores = true;
        for (const Smile::FGpuSceneHandle& Novo : CenaNova)
            for (const Smile::FGpuSceneHandle& Velho : CenaAntiga)
                if (Novo.Slot == Velho.Slot && Novo.Generation <= Velho.Generation)
                    TodasMaiores = false;
        Check(TodasMaiores, "todo slot reusado volta com generation maior");
    }

    // Duplicata recebe residencia PROPRIA.
    //
    // A copia carrega o GpuHandle da fonte por valor, e se o AddRenderable nao o substituisse os
    // dois renderaveis apontariam para o mesmo slot: o primeiro a ser removido aposentaria a
    // residencia do outro, que continuaria na cena apontando para um slot que a GPU pode ja ter
    // dado a um terceiro objeto.
    void TestDuplicataRecebeResidenciaPropria() {
        std::cout << "  duplicata recebe residencia propria\n";
        Smile::FScene Scene;
        const Smile::FRenderable& Fonte = Scene.AddRenderable(Make("fonte", 1));
        const Smile::FGpuSceneHandle HandleFonte = Fonte.GpuHandle;
        const Smile::u64 IdFonte = Fonte.Id;

        const Smile::FRenderable* Copia = Scene.DuplicateRenderable(IdFonte);
        Check(Copia != nullptr, "duplicar falhou");
        if (!Copia) return;

        Check(Copia->GpuHandle != HandleFonte,
              "a copia nao herda o handle da fonte");
        Check(Copia->GpuHandle.Slot != HandleFonte.Slot,
              "e nem o slot: sao residencias distintas");
        Check(Scene.GpuSlots().IsAlive(HandleFonte) && Scene.GpuSlots().IsAlive(Copia->GpuHandle),
              "as duas residencias estao vivas ao mesmo tempo");

        // Remover a fonte nao pode derrubar a residencia da copia.
        const Smile::FGpuSceneHandle HandleCopia = Copia->GpuHandle;
        Check(Scene.RemoveRenderable(IdFonte, kSemGpu), "remover a fonte falhou");
        Check(!Scene.GpuSlots().IsAlive(HandleFonte), "a residencia da fonte foi aposentada");
        Check(Scene.GpuSlots().IsAlive(HandleCopia), "a da copia continua viva");
    }

    // Remocao aposenta a residencia, e nao a deixa Live para sempre.
    void TestRemocaoAposentaAResidencia() {
        std::cout << "  remocao aposenta a residencia\n";
        Smile::FScene Scene;
        std::vector<Smile::u64> Ids;
        std::vector<Smile::FGpuSceneHandle> Handles;
        for (int i = 0; i < 4; ++i) {
            const Smile::FRenderable& R = Scene.AddRenderable(Make(("R" + std::to_string(i)).c_str(), i + 1));
            Ids.push_back(R.Id);
            Handles.push_back(R.GpuHandle);
        }
        Check(Scene.GpuSlots().LiveCount() == 4, "quatro residencias vivas");

        Check(Scene.RemoveRenderable(Ids[1], kSemGpu), "remocao falhou");
        Check(!Scene.GpuSlots().IsAlive(Handles[1]), "a residencia do removido morreu");
        Check(Scene.GpuSlots().LiveCount() == 3, "sobram tres vivas");
        for (int i : { 0, 2, 3 })
            Check(Scene.GpuSlots().IsAlive(Handles[i]), "as outras nao foram afetadas");

        // E o slot volta a ser usavel depois do Collect, sem o pool crescer.
        const Smile::u32 CapacidadeAntes = Scene.GpuSlots().Capacity();
        Scene.GpuSlots().Collect(kSemGpu);
        const Smile::FRenderable& Novo = Scene.AddRenderable(Make("novo", 9));
        Check(Scene.GpuSlots().Capacity() == CapacidadeAntes,
              "o slot aposentado foi reusado, sem crescer o pool");
        Check(Novo.GpuHandle.Slot == Handles[1].Slot,
              "e e exatamente o slot que vagou");
        Check(Novo.GpuHandle.Generation > Handles[1].Generation,
              "com generation maior");
    }

    // Pool no teto: o objeto entra na cena SEM residencia, e isso e visivel.
    void TestSemSlotDisponivel() {
        std::cout << "  objeto sem slot fica sem residencia, de forma visivel\n";
        Smile::FScene Scene;
        Smile::FGpuSceneSlotsTestAccess::SetMaxSlots(Scene.GpuSlots(), 2);

        const Smile::FGpuSceneHandle A = Scene.AddRenderable(Make("A", 1)).GpuHandle;
        const Smile::FGpuSceneHandle B = Scene.AddRenderable(Make("B", 2)).GpuHandle;
        Check(Scene.GpuSlots().IsAlive(A) && Scene.GpuSlots().IsAlive(B), "os dois cabem");

        const Smile::FRenderable& C = Scene.AddRenderable(Make("C", 3));
        Check(C.GpuHandle.Slot == Smile::FGpuSceneHandle::kInvalidSlot,
              "o terceiro entra com handle invalido");
        Check(!Scene.GpuSlots().IsAlive(C.GpuHandle), "que nao esta vivo");
        Check(Scene.GpuSlots().AllocationFailures() == 1,
              "a recusa e contada: nao pode ser silenciosa");

        // O objeto EXISTE na cena mesmo assim -- a FScene nao mente sobre o que tem.
        Check(Scene.Renderables().size() == 3, "o objeto entrou na cena");
        Check(Scene.IndexOfRenderable(C.Id) == 2, "e resolve por Id normalmente");

        // E removar um objeto sem residencia nao pode corromper contadores.
        const Smile::u32 VivosAntes = Scene.GpuSlots().LiveCount();
        Check(Scene.RemoveRenderable(C.Id, kSemGpu), "remover o sem-residencia falhou");
        Check(Scene.GpuSlots().LiveCount() == VivosAntes,
              "remover quem nao tinha residencia nao mexe na contagem de vivos");
    }

    // Ciclo remover->adicionar SEM Collect faz o pool crescer indefinidamente.
    //
    // Este teste existe para documentar por que a coleta e POR FRAME no renderer, e nao so no
    // reload. O Retire acontece em toda remocao; se o Collect so rodasse ao carregar outra cena,
    // cada ciclo tomaria um slot novo e a capacidade so encolheria no proximo load -- numa sessao
    // de edicao longa isso e crescimento sem teto, invisivel ate virar memoria a mais.
    void TestCicloSemCollectCresceOPool() {
        std::cout << "  ciclo sem Collect cresce o pool (por isso a coleta e por frame)\n";
        Smile::FScene Scene;
        Smile::u64 Id = Scene.AddRenderable(Make("inicial", 1)).Id;
        Check(Scene.GpuSlots().Capacity() == 1, "um slot");

        // Sem Collect entre os ciclos: cada remocao deixa um slot em Retired, e a adicao
        // seguinte nao pode reusa-lo.
        for (int i = 0; i < 5; ++i) {
            Check(Scene.RemoveRenderable(Id, kSemGpu), "remocao falhou");
            Id = Scene.AddRenderable(Make("novo", 2)).Id;
        }
        Check(Scene.GpuSlots().Capacity() == 6,
              "cinco ciclos sem Collect deixam seis slots: o pool cresceu");
        Check(Scene.GpuSlots().RetiredCount() == 5,
              "e cinco continuam presos em Retired");

        // Com o Collect, o mesmo ciclo nao cresce nada -- que e o que o renderer faz por frame.
        Smile::FScene Outra;
        Smile::u64 IdB = Outra.AddRenderable(Make("inicial", 1)).Id;
        for (int i = 0; i < 5; ++i) {
            Check(Outra.RemoveRenderable(IdB, kSemGpu), "remocao falhou");
            Outra.GpuSlots().Collect(kSemGpu);
            IdB = Outra.AddRenderable(Make("novo", 2)).Id;
        }
        Check(Outra.GpuSlots().Capacity() == 1,
              "com Collect a cada ciclo, o pool fica em um slot");
        Check(Outra.GpuSlots().RetiredCount() == 0, "e nada fica preso");
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
    TestClearNaoRessuscitaHandleDeGpu();
    TestDuplicataRecebeResidenciaPropria();
    TestRemocaoAposentaAResidencia();
    TestSemSlotDisponivel();
    TestCicloSemCollectCresceOPool();

    if (Failures == 0) {
        std::cout << "  OK\n";
        return 0;
    }
    std::cerr << "  " << Failures << " falha(s)\n";
    return 1;
}
