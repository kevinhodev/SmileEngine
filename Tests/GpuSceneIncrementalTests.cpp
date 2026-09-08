// Custo do produtor da tabela de instancias (Fase 4 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir: antes da Fase 4, `RecordUpload` descobria as linhas sujas
// com quatro `DiffTableRows` sobre vetores dimensionados pela CAPACIDADE da tabela. As linhas
// sujas ja eram poucas — o gate da Fase 2 provava isso —, mas o CUSTO DE DESCOBRI-LAS era a tabela
// inteira. Arrastar um objeto no gizmo com a sintetica de 1M carregada fazia ~191 MB de memcmp por
// frame para achar UMA linha, e nenhum contador publicado dizia isso: `rowsDirty` valia 1 e parecia
// otimo.
//
// O gate da fase — "1M instancias nao exigem reconstrucao/upload integral por frame" — e
// exatamente este numero. Por isso o que esta sob teste aqui e `LastRowsScanned()` — contado de
// DENTRO do laco do diff, uma unidade por (slot, coluna) —, e nao
// `LastDirtyRows()`: reintroduzir o diff de capacidade inteira mantem TODOS os outros contadores
// corretos e so move este.
//
// Precisa de device e command list porque a tabela cria buffers de verdade e grava
// CopyBufferRegion. A list e gravada e NUNCA executada: o que esta sob teste e a contabilidade do
// produtor, nao a copia. WARP basta.

#include <iostream>
#include <string>
#include <string_view>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"
#include "Smile/Scene/Scene.h"

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

    // Numero de renderaveis. Alto o suficiente para que "proporcional ao total" e "proporcional ao
    // que mudou" nao possam ser confundidos por folga de capacidade.
    constexpr u32 kObjects = 4096;

    struct FRig {
        Microsoft::WRL::ComPtr<ID3D12Device>              Device;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    Allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> List;
        FTextureSRVHeap                                   Heap;
        FScene                                            Scene;
        FGpuMeshTable                                     Meshes;
        FGpuMaterialTable                                 Materials;
        FGpuSceneInstanceTable                            Instances;

        bool Build() {
            Microsoft::WRL::ComPtr<IDXGIFactory4> Factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)))) return false;
            Microsoft::WRL::ComPtr<IDXGIAdapter> Warp;
            if (FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)))) return false;
            if (FAILED(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&Device))))
                return false;
            if (FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&Allocator))))
                return false;
            if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 Allocator.Get(), nullptr, IID_PPV_ARGS(&List))))
                return false;
            Heap.Initialize(Device.Get());

            // Sem malha e sem material de proposito: as duas tabelas resolvem ponteiro nulo para a
            // linha 0 reservada, e o que esta sob teste e a coluna de INSTANCIA.
            //
            // A residencia vem do proprio AddRenderable. Alocar um segundo slot por cima —
            // `Added.GpuHandle = GpuSlots().Allocate()` — deixaria o primeiro Live e sem dono: o
            // pool ficaria com o dobro dos vivos, e a tabela seria dimensionada sobre uma
            // capacidade inflada pelo vazamento em vez de pela folga do TableCapacityFor.
            for (u32 I = 0; I < kObjects; ++I) {
                FRenderable R;
                R.Transform.Position = { static_cast<f32>(I), 0.0f, 0.0f };
                R.RefreshWorldBounds();
                const FRenderable& Added = Scene.AddRenderable(R);
                if (Added.GpuHandle.Slot == FGpuSceneHandle::kInvalidSlot) return false;
            }
            Instances.SetupForScene(Device.Get(), Heap, Scene);
            return Instances.IsBuilt();
        }

        // Um ciclo de frame do renderer: produtor, depois upload.
        void Frame() {
            Instances.PrepareFrame(Scene, Meshes, Materials);
            Instances.RecordUpload(List.Get(), 0);
        }
    };
}

int main() {
    std::cout << "Smile.GpuSceneIncremental\n";

    FRig Rig;
    if (!Rig.Build()) {
        std::cerr << "  FAIL: aparelho WARP nao pode ser construido\n";
        return 1;
    }

    // --- primeira publicacao: toda linha viva sobe, e o diff visita exatamente elas -------------
    //
    // Nao a capacidade: a folga do TableCapacityFor existe para absorver carga aditiva, e visitar
    // slots que nunca tiveram ocupante e trabalho sobre nada.
    // Um slot por renderavel, nem mais. E o denominador de tudo o que vem abaixo: com o pool
    // inflado, a folga de capacidade da tabela viria de um vazamento e nao do TableCapacityFor, e
    // "a capacidade tem folga" passaria por acidente.
    CheckEq(Rig.Scene.GpuSlots().LiveCount(),
            static_cast<u32>(Rig.Scene.Renderables().size()),
            "o pool tem exatamente um slot vivo por renderavel");
    CheckEq(Rig.Scene.GpuSlots().RetiredCount(), 0u, "e nenhum aposentado antes da remocao");

    Rig.Frame();
    CheckEq(Rig.Instances.LiveRows(), kObjects, "a tabela recebe uma linha por renderavel vivo");
    CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns * kObjects,
            "a primeira publicacao visita as linhas vivas");
    CheckEq(Rig.Instances.LastDirtyRows(), kObjects, "a primeira publicacao sobe as linhas vivas");
    Check(Rig.Instances.Capacity() > kObjects, "a capacidade tem folga sobre as linhas vivas");

    // --- assentamento: o previous transform alcanca o atual e a tabela para ---------------------
    //
    // O primeiro frame deixa NeedsSettle_ ligado quando current != previous. Rodar ate parar e o
    // que o gate chama de "depois do assentamento".
    for (u32 I = 0; I < 4; ++I) Rig.Frame();
    CheckEq(Rig.Instances.LastRowsScanned(), 0u, "cena assentada nao visita linha nenhuma");
    CheckEq(Rig.Instances.LastDirtyRows(), 0u, "cena assentada nao sobe linha nenhuma");

    // --- mover UM objeto -----------------------------------------------------------------------
    //
    // A PROPRIEDADE DA FASE. `rowsDirty` == 1 ja valia antes; o que a Fase 4 acrescenta e que o
    // custo de chegar a esse 1 tambem e 1, e nao a capacidade da tabela.
    {
        FRenderable& R = Rig.Scene.Renderables()[1234];
        R.Transform.Position.Y += 5.0f;
        R.RefreshWorldBounds();
        Rig.Scene.BumpTransformsVersion(1234u);
        Rig.Frame();
        CheckEq(Rig.Instances.LastDirtyRows(), 1u, "mover um objeto suja uma linha");
        CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns,
                "mover um objeto visita uma linha, nao a capacidade");
    }

    // O frame seguinte assenta o previous transform daquele objeto — uma linha outra vez, nunca a
    // tabela. Este e o passo que um diff de capacidade inteira tambem pagaria por completo.
    Rig.Frame();
    CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns,
            "o assentamento visita a mesma unica linha");
    Rig.Frame();
    CheckEq(Rig.Instances.LastRowsScanned(), 0u, "depois do assentamento a tabela volta a zero");

    // --- indice REPETIDO na caixa de correio ----------------------------------------------------
    //
    // Mover um objeto ESTATICO bumpa transform E static casters, e os dois marcam o mesmo
    // renderavel: a caixa chega com o indice duas vezes. E o caso comum, nao uma borda.
    //
    // A segunda escrita da mesma linha nao seria redundante, seria DESTRUTIVA: `Previous` sai de
    // TransformRows_[Slot], que a primeira acabou de igualar a `Current`. O objeto perderia o
    // passado no exato frame em que se moveu — motion vector zero, sem erro nenhum, so o objeto
    // parando de borrar. Por isso o que se confere aqui sao as MATRIZES, e nao o contador: uma
    // deduplicacao que rodasse tarde demais deixaria os contadores certos e as matrizes erradas.
    {
        constexpr u32 kIdx = 77u;
        FRenderable& R = Rig.Scene.Renderables()[kIdx];
        const Mat44 Antes = R.Transform.Matrix();
        R.Transform.Position.X += 13.0f;
        R.RefreshWorldBounds();
        Rig.Scene.BumpTransformsVersion(kIdx);
        Rig.Scene.BumpStaticCastersVersion(kIdx); // o segundo bump, com o MESMO indice
        Rig.Frame();

        Mat44 Atual{}, Anterior{};
        Check(Rig.Instances.TransformsFor(R.GpuHandle, Atual, Anterior),
              "a linha do objeto movido continua resolvivel");
        Check(Atual.M[3][0] != Anterior.M[3][0],
              "indice repetido nao pode colapsar previous em current");
        Check(Anterior.M[3][0] == Antes.M[3][0],
              "o previous continua sendo o transform de antes do movimento");
        CheckEq(Rig.Instances.LastDirtyRows(), 1u, "e ainda e uma linha suja so");
        CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns,
                "o indice repetido nao dobra o trabalho do diff");
    }
    for (u32 I = 0; I < 3; ++I) Rig.Frame(); // assenta

    // --- transform storm: proporcional ao que se mexeu -----------------------------------------
    //
    // Um oitavo da cena em movimento. O numero visitado tem de acompanhar quem se mexeu e nao o
    // total; sem isto, o storm de 1M da sintetica volta a custar a tabela inteira por frame.
    {
        constexpr u32 kMoved = kObjects / 8u;
        for (u32 I = 0; I < kMoved; ++I) {
            FRenderable& R = Rig.Scene.Renderables()[I];
            R.Transform.Position.Z += 1.0f;
            R.RefreshWorldBounds();
            Rig.Scene.BumpTransformsVersion(I);
        }
        Rig.Frame();
        CheckEq(Rig.Instances.LastDirtyRows(), kMoved, "o storm suja exatamente quem se mexeu");
        CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns * kMoved,
                "o storm visita exatamente quem se mexeu");
    }

    // --- bump SEM dica cai na varredura completa ------------------------------------------------
    //
    // A fronteira do desenho, e ela e deliberada. A dica e opcional: quem bumpa sem dizer quem
    // mudou continua correto e paga uma varredura. O erro so pode cair para o lado de trabalho a
    // mais — uma linha desatualizada nunca. Se este teste passar a ver "1", a dica virou
    // obrigatoria em silencio e todo call site esquecido vira bug visual.
    for (u32 I = 0; I < 4; ++I) Rig.Frame(); // assenta o storm
    {
        FRenderable& R = Rig.Scene.Renderables()[99];
        R.Transform.Position.Y += 2.0f;
        R.RefreshWorldBounds();
        Rig.Scene.BumpTransformsVersion(); // sem indice
        Rig.Frame();
        CheckEq(Rig.Instances.LastDirtyRows(), 1u, "sem dica, ainda so a linha certa sobe");
        CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns * kObjects,
                "sem dica, o produtor varre a cena — conservador, e nao errado");
    }

    // Uma dica NAO impede a varredura quando outra mudanca ja a invalidou no mesmo intervalo: a
    // caixa e por intervalo entre publicacoes, nao por evento.
    for (u32 I = 0; I < 4; ++I) Rig.Frame();
    {
        Rig.Scene.Renderables()[10].Transform.Position.Y += 1.0f;
        Rig.Scene.Renderables()[10].RefreshWorldBounds();
        Rig.Scene.BumpTransformsVersion(10u);
        Rig.Scene.Renderables()[11].Transform.Position.Y += 1.0f;
        Rig.Scene.Renderables()[11].RefreshWorldBounds();
        Rig.Scene.BumpTransformsVersion(); // invalida a caixa inteira
        Rig.Frame();
        CheckEq(Rig.Instances.LastDirtyRows(), 2u, "as duas linhas mudadas sobem");
        CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns * kObjects,
                "um bump cego invalida as dicas do mesmo intervalo");
    }

    // --- remover um objeto: a linha dele e zerada, e ela tambem e visitada ----------------------
    //
    // O caso que o conjunto tocado nao pode esquecer. Um slot que sai da cena precisa da copia da
    // linha ZERADA; sem ele no conjunto, o objeto removido continuaria residente na tabela GPU ate
    // alguem reusar o slot — e o leitor validaria a generation contra uma linha que ainda a tem.
    //
    // A varredura aqui e completa DE PROPOSITO: remover desloca os indices da cauda, e a caixa de
    // correio guarda indice. Por isso a remocao invalida a dica na FScene, e nao no chamador.
    for (u32 I = 0; I < 4; ++I) Rig.Frame();
    {
        const u64 Id = Rig.Scene.Renderables()[7].Id;
        const FGpuSceneHandle Removed = Rig.Scene.Renderables()[7].GpuHandle;
        FGpuSceneInstanceMeta Meta{};
        Check(Rig.Instances.MetaFor(Removed, Meta), "a linha do objeto existe antes da remocao");

        // Pelo caminho de PRODUCAO, sem aposentar a residencia na mao antes. Um `Retire` avulso
        // aqui e idempotente — o RemoveRenderable simplesmente nao acharia o handle vivo —, e por
        // isso mesmo mascararia o que interessa: o teste passaria identico se o RemoveRenderable
        // parasse de aposentar a residencia, e o vazamento de slot so apareceria como pool
        // crescendo meses depois.
        Check(Rig.Scene.RemoveRenderable(Id, FGpuFenceSet{}), "o renderavel sai da cena");
        CheckEq(Rig.Scene.GpuSlots().LiveCount(),
                static_cast<u32>(Rig.Scene.Renderables().size()),
                "remover pela API de producao aposenta a residencia junto");
        CheckEq(Rig.Scene.GpuSlots().RetiredCount(), 1u, "e o slot vai para aposentado, nao a lixo");

        Rig.Frame();
        CheckEq(Rig.Instances.LiveRows(), kObjects - 1u, "a tabela perde a linha viva");
        CheckEq(Rig.Instances.LastDirtyRows(), 1u, "remover um objeto suja so a linha dele");
        Check(!Rig.Instances.MetaFor(Removed, Meta),
              "a linha zerada deixa de responder pela generation antiga");
    }

    // --- teto da caixa de correio ---------------------------------------------------------------
    //
    // Acima do teto a dica se recusa a crescer e devolve a varredura. Sem isso, uma cena com
    // milhoes de objetos em movimento carregaria uma lista maior que a propria cena.
    //
    // Duas marcacoes por objeto: repetir conta para o teto. Nao ha deduplicacao na marcacao de
    // proposito — ela roda no caminho de edicao, e um `find` por marca seria pior que a varredura
    // que a caixa evita.
    for (u32 I = 0; I < 4; ++I) Rig.Frame();
    {
        const u32 Live = static_cast<u32>(Rig.Scene.Renderables().size());
        for (u32 I = 0; I < Live; ++I) {
            Rig.Scene.Renderables()[I].Transform.Position.Y += 0.5f;
            Rig.Scene.Renderables()[I].RefreshWorldBounds();
            Rig.Scene.BumpTransformsVersion(I);
            Rig.Scene.BumpTransformsVersion(I);
        }
        Check(!Rig.Scene.DirtyHintComplete(), "a caixa se declara incompleta ao estourar o teto");
        Rig.Frame();
        CheckEq(Rig.Instances.LastDirtyRows(), Live, "todas as linhas mudadas sobem");
        CheckEq(Rig.Instances.LastRowsScanned(), FGpuSceneInstanceTable::kColumns * Live,
                "estourar o teto cai na varredura, que aqui e a cena inteira");
    }

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
}
