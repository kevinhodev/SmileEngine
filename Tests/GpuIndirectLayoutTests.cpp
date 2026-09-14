// QUANDO o layout de regioes e refeito, sobre tabelas e cena de verdade (Fase 5 do GPU-DRIVEN-PLAN).
//
// O contrato: o layout so e recontado quando as linhas PUBLICADAS mudam o que a capacidade conta —
// ocupacao, associacao, classe, pool, `RaytracingOnly` e mascara de malha inutilizavel. Transform,
// assentamento, visibilidade e mobilidade sozinhos nao o refazem, e uma edicao de cor tambem nao.
//
// O `TestarChavesDoLayout` (Smile.GpuClassifyWarp) prova que a chave e completa, bit a bit. O que so
// este teste prova e que as tabelas de PRODUCAO a consultam no ponto certo — antes do diff, que apaga
// o valor antigo —, e que os caminhos que o editor usa (bump da cena, `MarkDirty` do material, Setup
// da malha) movem ou nao movem a versao. Cada evento que NAO deve reconstruir confere tambem que a
// tabela publicou alguma linha: sem isso, "nao reconstruiu" passaria porque nada chegou a acontecer.
//
// Device WARP porque as tabelas criam buffers e gravam CopyBufferRegion, e porque um FGpuMesh so e
// valido apontando para um pool D3D12. A command list e gravada e nunca executada.

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Scene/GpuIndirectLayoutState.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"
#include "Smile/Scene/Scene.h"

using namespace Smile;
namespace wrl = Microsoft::WRL;

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

    void CheckEq64(u64 _Got, u64 _Want, std::string_view _Message) {
        if (_Got != _Want) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << " (obtido " << _Got << ", esperado " << _Want
                      << ")\n";
        }
    }

    struct FRig {
        wrl::ComPtr<ID3D12Device>              Device;
        wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
        wrl::ComPtr<ID3D12GraphicsCommandList> List;
        FTextureSRVHeap                        Heap;
        FScene                                 Scene;

        wrl::ComPtr<ID3D12Resource> PoolA, PoolB;
        FGpuMesh  MalhaA, MalhaB;
        FMaterial Opaco, Mascarado, Troca;

        FGpuMeshTable          Meshes;
        FGpuMaterialTable      Materials;
        FGpuSceneInstanceTable Instances;
        FGpuIndirectLayoutState Layout;
        u32 Motivo = 0;

        // Ids dos renderaveis, porque remover desloca os indices da cauda.
        u64 IdOpaco[3]{}, IdTroca[2]{}, IdMascarado = 0, IdOpacoB = 0;

        bool Build() {
            wrl::ComPtr<IDXGIFactory4> Factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)))) return false;
            wrl::ComPtr<IDXGIAdapter> Warp;
            if (FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)))) return false;
            if (FAILED(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device))))
                return false;
            if (FAILED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&Allocator))))
                return false;
            if (FAILED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                                 nullptr, IID_PPV_ARGS(&List))))
                return false;
            Heap.Initialize(Device.Get());

            // Dois pools, uma malha em cada: o par VB/IB e a identidade do pool.
            PoolA = FGpuMesh::CreatePoolBuffer(Device.Get(), 64 * 1024);
            PoolB = FGpuMesh::CreatePoolBuffer(Device.Get(), 64 * 1024);
            if (!PoolA || !PoolB) return false;
            MalhaA.InitFromPool(PoolA, 0, 32 * 1024, 48 * 1024, 3, 3, 1);
            MalhaB.InitFromPool(PoolB, 0, 32 * 1024, 48 * 1024, 3, 3, 1);
            if (!MalhaA.IsValid() || !MalhaB.IsValid()) return false;
            Mascarado.Constants.AlphaTest = 1u;

            auto Add = [&](FGpuMesh* _Malha, FMaterial* _Material) {
                FRenderable R;
                R.Mesh     = _Malha;
                R.Material = _Material;
                return Scene.AddRenderable(R).Id;
            };
            for (u64& Id : IdOpaco) Id = Add(&MalhaA, &Opaco);
            for (u64& Id : IdTroca) Id = Add(&MalhaB, &Troca);
            IdMascarado = Add(&MalhaA, &Mascarado);
            IdOpacoB    = Add(&MalhaB, &Opaco);

            Meshes.SetupForScene(Device.Get(), Heap, Scene);
            FMaterial* Lista[3]{ &Opaco, &Mascarado, &Troca };
            Materials.SetupForMaterials(Device.Get(), Heap, Lista);
            Instances.SetupForScene(Device.Get(), Heap, Scene);
            return Instances.IsBuilt() && Meshes.GeometryPools().size() == 2u;
        }

        // Um frame, na ORDEM do RendererFrame: produtor, os tres uploads, e so entao o layout — que le
        // as linhas que acabaram de ser publicadas.
        void Frame() {
            Instances.PrepareFrame(Scene, Meshes, Materials);
            Materials.RecordUpload(List.Get(), 0);
            Meshes.RecordUpload(List.Get(), 0);
            Instances.RecordUpload(List.Get(), 0);
            Consultar();
        }

        // O layout lido sem produtor e sem upload. Serve para olhar a janela entre um Setup (ou Release) e
        // a publicacao seguinte, em que as linhas publicadas estao VAZIAS: a versao tem de acompanhar as
        // linhas nos dois pontos, e nao so no upload.
        void Consultar() {
            const FGpuLayoutInputs In{
                Instances.PublishedMeta(), Materials.PublishedRows(), Meshes.PublishedRows(),
                static_cast<u32>(Meshes.GeometryPools().size()), Instances.LayoutInputsVersion(),
                Materials.LayoutInputsVersion(), Meshes.LayoutInputsVersion()
            };
            Motivo = Layout.Refresh(In);
        }

        FRenderable& R(u64 _Id) { return *Scene.FindRenderable(_Id); }
        u32 Indice(u64 _Id) const { return static_cast<u32>(Scene.IndexOfRenderable(_Id)); }

        // Capacidade da classe opaca one-sided no pool.
        u32 Cap(u32 _Pool) const {
            const auto Regioes = Layout.Layout().Regions();
            return _Pool < Regioes.size() ? Regioes[_Pool].Capacity : 0xFFFFFFFFu;
        }
    };
}

int main() {
    std::cout << "Smile.GpuIndirectLayout\n";

    auto Rig = std::make_unique<FRig>();
    if (!Rig->Build()) {
        std::cerr << "  FAIL: aparelho WARP nao pode ser construido\n";
        return 1;
    }
    FRig& G = *Rig;

    // --- a primeira publicacao monta o layout ---------------------------------------------------
    G.Frame();
    CheckEq(G.Motivo, GpuLayoutRefreshOk, "a primeira publicacao monta o layout");
    CheckEq64(G.Layout.RebuildAttempts(), 1ull, "uma contagem");
    CheckEq(G.Cap(0), 3u, "pool A: os tres opacos");
    CheckEq(G.Cap(1), 3u, "pool B: os dois da troca e o opaco — o mascarado nao conta");
    CheckEq(static_cast<u32>(G.Instances.PublishedMeta().size()), G.Instances.Capacity(),
            "a meta publicada cobre a tabela inteira");
    CheckEq(static_cast<u32>(G.Materials.PublishedRows().size()), 4u,
            "o material publicado tem a linha default e os tres");

    // --- os eventos que NAO reconstroem ---------------------------------------------------------
    for (u32 I = 0; I < 4; ++I) G.Frame();
    CheckEq64(G.Layout.RebuildAttempts(), 1ull, "cena assentada nao reconta");

    {
        FRenderable& R = G.R(G.IdOpaco[0]);
        R.Transform.Position.X += 3.0f;
        R.RefreshWorldBounds();
        G.Scene.BumpTransformsVersion(G.Indice(G.IdOpaco[0]));
        G.Frame();
        Check(G.Instances.LastDirtyRows() > 0u, "transform: a tabela publicou a linha");
        CheckEq64(G.Layout.RebuildAttempts(), 1ull, "transform nao reconta");
        G.Frame();
        Check(G.Instances.LastDirtyRows() > 0u, "assentamento: a tabela publicou a linha");
        CheckEq64(G.Layout.RebuildAttempts(), 1ull, "assentamento nao reconta");
    }
    {
        G.R(G.IdOpaco[1]).Visible = false;
        G.Scene.BumpStaticCastersVersion(G.Indice(G.IdOpaco[1]));
        G.Frame();
        Check(G.Instances.LastDirtyRows() > 0u, "visibilidade: a tabela publicou a linha");
        CheckEq64(G.Layout.RebuildAttempts(), 1ull, "visibilidade nao reconta");
        CheckEq(G.Cap(0), 3u, "e o escondido continua reservando espaco");
    }
    {
        G.R(G.IdOpaco[2]).Mobility = EMobility::Dynamic;
        G.Scene.BumpStaticCastersVersion(G.Indice(G.IdOpaco[2]));
        G.Frame();
        Check(G.Instances.LastDirtyRows() > 0u, "mobilidade: a tabela publicou a linha");
        CheckEq64(G.Layout.RebuildAttempts(), 1ull, "mobilidade nao reconta");
    }
    {
        G.Opaco.Constants.BaseColorFactor = { 0.2f, 0.4f, 0.6f, 1.0f };
        G.Materials.MarkDirty();
        G.Frame();
        CheckEq(G.Materials.LastDirtyRows(), 1u, "cor: o material publicou a linha");
        CheckEq64(G.Layout.RebuildAttempts(), 1ull, "editar a cor nao reconta");
    }

    // --- os eventos que reconstroem -------------------------------------------------------------
    {
        G.Troca.TwoSided = true;
        G.Materials.MarkDirty();
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 2ull, "classe: virar two-sided reconta");
        CheckEq(G.Cap(1), 1u, "e a troca sai do pool B");
        G.Troca.TwoSided = false;
        G.Materials.MarkDirty();
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 3ull, "classe: voltar reconta");
        CheckEq(G.Cap(1), 3u, "e a troca volta");
    }
    {
        G.R(G.IdTroca[0]).RaytracingOnly = true;
        G.Scene.BumpStaticCastersVersion(G.Indice(G.IdTroca[0]));
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 4ull, "RaytracingOnly reconta");
        CheckEq(G.Cap(1), 2u, "e a linha sai da contagem");
    }
    {
        G.R(G.IdTroca[1]).Mesh = &G.MalhaA;
        G.Scene.BumpStaticCastersVersion(G.Indice(G.IdTroca[1]));
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 5ull, "associacao: trocar de malha reconta");
        CheckEq(G.Cap(0), 4u, "e a instancia passa ao pool A");
        CheckEq(G.Cap(1), 1u, "saindo do pool B");
    }
    {
        // Ocupacao de uma instancia que NAO conta (mascarada): reconta mesmo sem mudar a capacidade.
        // A chave e a ocupacao, e nao o efeito dela.
        Check(G.Scene.RemoveRenderable(G.IdMascarado, FGpuFenceSet{}), "o mascarado sai da cena");
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 6ull, "ocupacao: remover reconta");
        CheckEq(G.Cap(0), 4u, "e a capacidade nao muda");
    }
    {
        FRenderable Novo;
        Novo.Mesh     = &G.MalhaA;
        Novo.Material = &G.Opaco;
        G.Scene.AddRenderable(Novo);
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 7ull, "ocupacao: acrescentar reconta");
        CheckEq(G.Cap(0), 5u, "e o novo entra no pool A");
    }
    {
        // A malha B passa a morar no pool A. Pool so muda por Setup da tabela de malhas.
        G.MalhaB.InitFromPool(G.PoolA, 3200, 40 * 1024, 56 * 1024, 3, 3, 1);
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 8ull, "pool: mudar o pool de uma malha reconta");
        CheckEq(static_cast<u32>(G.Layout.Layout().Regions().size()), 4u, "com um pool so");
        CheckEq(G.Cap(0), 6u, "e o opaco da malha B entra no pool A");
    }
    {
        // IndexCount que nao descreve triangulos inteiros: a malha A fica fora do indireto. O numero de
        // pools NAO muda aqui, entao so a versao da tabela pode disparar as duas contagens.
        G.MalhaA.InitFromPool(G.PoolA, 0, 32 * 1024, 48 * 1024, 3, 4, 1);
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 9ull, "o Setup da malha, antes de publicar, ja reconta");
        CheckEq(G.Cap(0), 0u, "sobre as linhas de malha vazias, nenhuma instancia conta");
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 10ull, "e a publicacao das linhas novas reconta de novo");
        CheckEq(G.Cap(0), 1u, "mascara de malha inutilizavel: so o opaco da malha B conta");
    }
    {
        // A chave da malha e GROSSA de proposito (ver FGpuMeshTable::LayoutInputsVersion): um Setup que
        // republica as mesmas linhas tambem reconta. Custa uma contagem num caminho que ja drena.
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 11ull, "um Setup de malha sem mudanca tambem reconta");
    }
    {
        FMaterial* Lista[3]{ &G.Opaco, &G.Mascarado, &G.Troca };
        G.Materials.SetupForMaterials(G.Device.Get(), G.Heap, Lista);
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 12ull, "o Setup do material, antes de publicar, ja reconta");
        CheckEq(G.Cap(0), 0u, "sobre as linhas de material vazias, nenhuma instancia conta");
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 13ull, "e a publicacao reconta de novo");
        CheckEq(G.Cap(0), 1u, "com a contagem de antes");
    }
    {
        G.Instances.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 14ull, "o Setup das instancias, antes de publicar, ja reconta");
        CheckEq(G.Cap(0), 0u, "sobre a meta vazia, nenhuma instancia conta");
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 15ull, "e a publicacao reconta de novo");
        CheckEq(G.Cap(0), 1u, "com a contagem de antes");
    }

    for (u32 I = 0; I < 4; ++I) G.Frame();
    CheckEq64(G.Layout.RebuildAttempts(), 15ull, "depois dos eventos, a cena assentada nao reconta");
    CheckEq(G.Motivo, GpuLayoutRefreshOk, "e o layout continua valido");

    // --- Release: as linhas publicadas somem, e a versao acompanha ------------------------------
    G.Materials.Release(G.Heap);
    G.Consultar();
    CheckEq64(G.Layout.RebuildAttempts(), 16ull, "o Release do material reconta");
    CheckEq(G.Cap(0), 0u, "sem material publicado, nenhuma instancia conta");
    G.Instances.Release(G.Heap);
    G.Consultar();
    CheckEq64(G.Layout.RebuildAttempts(), 17ull, "o Release das instancias reconta");

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
}
