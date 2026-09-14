// QUANDO o layout de regioes e refeito, e o que as tabelas PUBLICAM, sobre tabelas, cena e fila de
// verdade (Fase 5 do GPU-DRIVEN-PLAN).
//
// Dois contratos:
//
//  1. O layout so e recontado quando as linhas publicadas mudam o que a capacidade conta — ocupacao,
//     associacao, classe, pool, `RaytracingOnly` e mascara de malha inutilizavel. Transform,
//     assentamento, visibilidade, mobilidade e cor nao o refazem. O `TestarChavesDoLayout`
//     (Smile.GpuClassifyWarp) prova que a chave e completa bit a bit; este prova que as tabelas de
//     PRODUCAO a consultam, pelos caminhos que o editor usa. Todo evento que NAO deve recontar confere
//     que a tabela publicou alguma linha: sem isso, "nao recontou" passaria por nada ter acontecido.
//
//  2. "Publicado" e o que a GPU tem. Cada frame e gravado, EXECUTADO e esperado pela fence, e as tres
//     tabelas sao lidas de volta no fim da propria lista — onde um leitor do frame estaria — e
//     comparadas byte a byte com as linhas publicadas no momento da gravacao. As copias recusadas
//     (lista nula) nao podem avancar nem as linhas publicadas nem a versao, e a nova tentativa tem de
//     publicar tudo. Dois frames gravados antes de qualquer um executar provam que cada upload le o
//     staging do SEU frame slot.
//
// A camada de debug participa: ERROR e CORRUPTION reprovam.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    void Hr(HRESULT _H, const char* _Onde) {
        if (FAILED(_H)) {
            char Buf[32];
            std::snprintf(Buf, sizeof(Buf), " falhou (0x%08X)", static_cast<unsigned>(_H));
            throw std::runtime_error(std::string(_Onde) + Buf);
        }
    }

    // Quais uploads o frame RECUSA. A lista nula e a recusa que a porta do armazenamento aplica.
    struct FRecusa {
        bool Material   = false;
        bool Malha      = false;
        bool Instancias = false;
    };

    // O que um frame tem de ler de volta: os bytes das linhas publicadas no momento da gravacao.
    struct FLeitura {
        const char*                 Nome = "";
        std::vector<u8>             Esperado;
        wrl::ComPtr<ID3D12Resource> Buffer;
    };

    std::vector<u8> BytesDe(std::span<const u8> _Bytes) { return { _Bytes.begin(), _Bytes.end() }; }

    template <typename TRow>
    std::span<const u8> ComoBytes(std::span<const TRow> _Linhas) {
        return { reinterpret_cast<const u8*>(_Linhas.data()), _Linhas.size() * sizeof(TRow) };
    }

    struct FRig {
        wrl::ComPtr<ID3D12Device>              Device;
        wrl::ComPtr<ID3D12InfoQueue>           Info;
        wrl::ComPtr<ID3D12CommandQueue>        Queue;
        wrl::ComPtr<ID3D12Fence>               Fence;
        u64                                    FenceValue = 0;
        wrl::ComPtr<ID3D12CommandAllocator>    Allocator[FGpuTableStorage::kFrameSlots];
        wrl::ComPtr<ID3D12GraphicsCommandList> List[FGpuTableStorage::kFrameSlots];
        FLeitura                               Leituras[FGpuTableStorage::kFrameSlots][3];
        u32                                    ProximoSlot = 0;

        FTextureSRVHeap Heap;
        FScene          Scene;

        wrl::ComPtr<ID3D12Resource> PoolA, PoolB;
        FGpuMesh  MalhaA, MalhaB;
        FMaterial Opaco, Mascarado, Troca;

        FGpuMeshTable           Meshes;
        FGpuMaterialTable       Materials;
        FGpuSceneInstanceTable  Instances;
        FGpuIndirectLayoutState Layout;
        u32 Motivo = 0;

        // Ids dos renderaveis, porque remover desloca os indices da cauda.
        u64 IdOpaco[3]{}, IdTroca[2]{}, IdMascarado = 0, IdOpacoB = 0;

        void Construir() {
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

            D3D12_COMMAND_QUEUE_DESC QD{};
            Hr(Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&Queue)), "CreateCommandQueue");
            Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence");
            for (u32 S = 0; S < FGpuTableStorage::kFrameSlots; ++S) {
                Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&Allocator[S])),
                   "CreateCommandAllocator");
                Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator[S].Get(),
                                             nullptr, IID_PPV_ARGS(&List[S])),
                   "CreateCommandList");
                Hr(List[S]->Close(), "Close inicial");
            }
            Heap.Initialize(Device.Get());

            // Dois pools, uma malha em cada: o par VB/IB e a identidade do pool.
            PoolA = FGpuMesh::CreatePoolBuffer(Device.Get(), 64 * 1024);
            PoolB = FGpuMesh::CreatePoolBuffer(Device.Get(), 64 * 1024);
            MalhaA.InitFromPool(PoolA, 0, 32 * 1024, 48 * 1024, 3, 3, 1);
            MalhaB.InitFromPool(PoolB, 0, 32 * 1024, 48 * 1024, 3, 3, 1);
            if (!MalhaA.IsValid() || !MalhaB.IsValid())
                throw std::runtime_error("as malhas do aparelho precisam ser validas");
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
            if (!Instances.IsBuilt() || Meshes.GeometryPools().size() != 2u)
                throw std::runtime_error("as tabelas do aparelho nao foram construidas");
            Drenar("construcao");
        }

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
                    std::cerr << "  FAIL: a camada de debug reprovou\n";
                }
            }
            Info->ClearStoredMessages();
        }

        // O layout, lido das linhas PUBLICADAS. Chamado pela gravacao depois dos uploads — a ordem do
        // RendererFrame —, e sozinho para olhar a janela entre um Setup (ou Release) e a publicacao.
        void Consultar() {
            const FGpuLayoutInputs In{
                Instances.PublishedMeta(), Materials.PublishedRows(), Meshes.PublishedRows(),
                static_cast<u32>(Meshes.GeometryPools().size()), Instances.LayoutInputsVersion(),
                Materials.LayoutInputsVersion(), Meshes.LayoutInputsVersion()
            };
            Motivo = Layout.Refresh(In);
        }

        // Grava um frame no slot: produtor, os tres uploads, o layout, e a leitura das tres tabelas no
        // FIM da lista, com a imagem esperada tirada das linhas publicadas neste instante.
        void Gravar(u32 _Slot, FRecusa _Recusa = {}) {
            Hr(Allocator[_Slot]->Reset(), "Reset do allocator");
            ID3D12GraphicsCommandList* L = List[_Slot].Get();
            Hr(L->Reset(Allocator[_Slot].Get(), nullptr), "Reset da lista");

            Instances.PrepareFrame(Scene, Meshes, Materials);
            Materials.RecordUpload(_Recusa.Material ? nullptr : L, _Slot);
            Meshes.RecordUpload(_Recusa.Malha ? nullptr : L, _Slot);
            Instances.RecordUpload(_Recusa.Instancias ? nullptr : L, _Slot);
            Consultar();

            auto Ler = [&](FLeitura& _Leitura, const char* _Nome, ID3D12Resource* _Tabela,
                           std::span<const u8> _Publicado) {
                _Leitura.Nome     = _Nome;
                _Leitura.Esperado = BytesDe(_Publicado);
                _Leitura.Buffer.Reset();
                if (!_Tabela || _Leitura.Esperado.empty()) return;

                D3D12_HEAP_PROPERTIES HP{};
                HP.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC RD{};
                RD.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
                RD.Width            = _Leitura.Esperado.size();
                RD.Height           = 1;
                RD.DepthOrArraySize = 1;
                RD.MipLevels        = 1;
                RD.SampleDesc.Count = 1;
                RD.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&_Leitura.Buffer)),
                   "CreateCommittedResource (leitura)");

                D3D12_RESOURCE_BARRIER B{};
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.pResource   = _Tabela;
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateBefore = FGpuTableStorage::kShaderReadState;
                B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                L->ResourceBarrier(1, &B);
                L->CopyBufferRegion(_Leitura.Buffer.Get(), 0, _Tabela, 0, _Leitura.Esperado.size());
                std::swap(B.Transition.StateBefore, B.Transition.StateAfter);
                L->ResourceBarrier(1, &B);
            };
            Ler(Leituras[_Slot][0], "meta", Instances.MetaResource(),
                ComoBytes(Instances.PublishedMeta()));
            Ler(Leituras[_Slot][1], "material", Materials.Resource(),
                ComoBytes(Materials.PublishedRows()));
            Ler(Leituras[_Slot][2], "malha", Meshes.Resource(), ComoBytes(Meshes.PublishedRows()));

            const HRESULT Fechou = L->Close();
            if (FAILED(Fechou)) Drenar("gravacao");
            Hr(Fechou, "Close");
        }

        // Executa a lista do slot, espera a fence e confere a imagem.
        void Executar(u32 _Slot) {
            ID3D12CommandList* Listas[] = { List[_Slot].Get() };
            Queue->ExecuteCommandLists(1, Listas);
            ++FenceValue;
            Hr(Queue->Signal(Fence.Get(), FenceValue), "Signal");
            if (Fence->GetCompletedValue() < FenceValue) {
                struct FEvento {
                    HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    ~FEvento() { if (Value) CloseHandle(Value); }
                } Evento;
                Hr(Fence->SetEventOnCompletion(FenceValue, Evento.Value), "SetEventOnCompletion");
                if (WaitForSingleObject(Evento.Value, 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error("a lista nao terminou");
            }
            Drenar("execucao");

            for (FLeitura& Lt : Leituras[_Slot]) {
                if (!Lt.Buffer) continue;
                u8* P = nullptr;
                Hr(Lt.Buffer->Map(0, nullptr, reinterpret_cast<void**>(&P)), "Map (leitura)");
                const bool Igual = std::memcmp(P, Lt.Esperado.data(), Lt.Esperado.size()) == 0;
                Lt.Buffer->Unmap(0, nullptr);
                Check(Igual, std::string("a GPU tem exatamente as linhas publicadas: ") + Lt.Nome);
            }
        }

        // Um frame, gravado e executado antes do seguinte.
        void Frame(FRecusa _Recusa = {}) {
            const u32 Slot = ProximoSlot;
            ProximoSlot    = (ProximoSlot + 1u) % FGpuTableStorage::kFrameSlots;
            Gravar(Slot, _Recusa);
            Executar(Slot);
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

int main() try {
    std::cout << "Smile.GpuIndirectLayout\n";

    auto Rig = std::make_unique<FRig>();
    Rig->Construir();
    FRig& G = *Rig;

    // --- a porta do armazenamento, direto -------------------------------------------------------
    //
    // As tabelas consultam a porta antes de gravar, entao pelo caminho delas a checagem de dentro do
    // `RecordUpload` nunca e alcancada. Ela e conferida aqui, sobre uma lista que nunca executa.
    {
        wrl::ComPtr<ID3D12CommandAllocator>    Alloc;
        wrl::ComPtr<ID3D12GraphicsCommandList> L;
        Hr(G.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Alloc)),
           "CreateCommandAllocator (porta)");
        Hr(G.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Alloc.Get(), nullptr,
                                       IID_PPV_ARGS(&L)),
           "CreateCommandList (porta)");
        FGpuTableStorage S;
        S.Create(G.Device.Get(), G.Heap, sizeof(u32), 4u, "porta");
        const u32 Linhas[5]{ 1u, 2u, 3u, 4u, 5u };
        const u32 Dentro[1]{ 3u };
        const u32 Fora[1]{ S.Capacity() };
        Check(S.AcceptsUpload(L.Get(), Linhas, {}), "porta: nada a copiar e aceito");
        Check(!S.AcceptsUpload(nullptr, Linhas, Dentro), "porta: lista nula recusa");
        Check(!S.AcceptsUpload(L.Get(), Linhas, Fora), "porta: linha fora do buffer recusa");
        Check(S.AcceptsUpload(L.Get(), Linhas, Dentro), "porta: linha dentro do buffer e aceita");
        CheckEq(S.RecordUpload(nullptr, 0u, Linhas, Dentro), 0u, "gravacao: lista nula nao grava");
        CheckEq(S.RecordUpload(L.Get(), 0u, Linhas, Fora), 0u,
                "gravacao: linha fora do buffer nao grava");
        L->Close();
        S.Release(G.Heap);
        G.Drenar("porta");
    }

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

    // --- os eventos que NAO recontam ------------------------------------------------------------
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

    // --- classe, com a copia RECUSADA primeiro --------------------------------------------------
    {
        const u32 LinhaTroca = 3u; // linha 0 e o default; Opaco, Mascarado, Troca
        G.Troca.TwoSided = true;
        G.Materials.MarkDirty();
        G.Frame({ .Material = true });
        CheckEq(G.Materials.LastDirtyRows(), 0u, "classe recusada: nenhuma linha publicada");
        CheckEq(G.Materials.PublishedRows()[LinhaTroca].PipelineClass,
                static_cast<u32>(GpuPipelineClassOpaqueOneSided),
                "classe recusada: a linha publicada continua a antiga");
        CheckEq64(G.Layout.RebuildAttempts(), 1ull, "classe recusada: a versao nao sobe e nada reconta");
        CheckEq(G.Cap(1), 3u, "classe recusada: a capacidade continua a da GPU");

        G.Frame();
        CheckEq(G.Materials.LastDirtyRows(), 1u, "classe: a nova tentativa publica");
        CheckEq(G.Materials.PublishedRows()[LinhaTroca].PipelineClass,
                static_cast<u32>(GpuPipelineClassOpaqueTwoSided), "classe: a linha nova esta publicada");
        CheckEq64(G.Layout.RebuildAttempts(), 2ull, "classe: virar two-sided reconta");
        CheckEq(G.Cap(1), 1u, "e a troca sai do pool B");

        G.Troca.TwoSided = false;
        G.Materials.MarkDirty();
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 3ull, "classe: voltar reconta");
        CheckEq(G.Cap(1), 3u, "e a troca volta");
    }

    // --- RaytracingOnly, com a copia das instancias RECUSADA e um ciclo novo do produtor no meio -
    {
        const u32 Slot = G.R(G.IdTroca[0]).GpuHandle.Slot;
        G.R(G.IdTroca[0]).RaytracingOnly = true;
        G.Scene.BumpStaticCastersVersion(G.Indice(G.IdTroca[0]));
        G.Frame({ .Instancias = true });
        CheckEq(G.Instances.LastDirtyRows(), 0u, "instancias recusadas: nenhuma linha publicada");
        Check((G.Instances.PublishedMeta()[Slot].Flags & GpuSceneInstanceRaytracingOnly) == 0u,
              "instancias recusadas: a linha publicada continua a antiga");
        CheckEq64(G.Layout.RebuildAttempts(), 3ull, "instancias recusadas: nada reconta");

        // Outro objeto se move ANTES da nova tentativa: o produtor abre um ciclo novo, e o slot que
        // ficou pendente nao pode ser esquecido.
        FRenderable& Outro = G.R(G.IdOpaco[2]);
        Outro.Transform.Position.Z += 1.0f;
        Outro.RefreshWorldBounds();
        G.Scene.BumpTransformsVersion(G.Indice(G.IdOpaco[2]));
        G.Frame();
        Check((G.Instances.PublishedMeta()[Slot].Flags & GpuSceneInstanceRaytracingOnly) != 0u,
              "RaytracingOnly: a nova tentativa publica a linha pendente");
        CheckEq64(G.Layout.RebuildAttempts(), 4ull, "RaytracingOnly reconta");
        CheckEq(G.Cap(1), 2u, "e a linha sai da contagem");
    }

    // --- associacao e ocupacao ------------------------------------------------------------------
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

    // --- pool, com a copia da malha RECUSADA ----------------------------------------------------
    {
        G.MalhaB.InitFromPool(G.PoolA, 3200, 40 * 1024, 56 * 1024, 3, 3, 1);
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Frame({ .Malha = true });
        CheckEq(G.Meshes.LastDirtyRows(), 0u, "malha recusada: nenhuma linha publicada");
        Check(G.Meshes.PublishedRows().empty(), "malha recusada: as linhas publicadas seguem vazias");
        CheckEq64(G.Layout.RebuildAttempts(), 8ull, "o Setup reconta, e a recusa nao acrescenta nada");
        CheckEq(G.Cap(0), 0u, "sobre as linhas de malha vazias, nenhuma instancia conta");

        G.Frame();
        Check(!G.Meshes.PublishedRows().empty(), "malha: a nova tentativa publica");
        CheckEq64(G.Layout.RebuildAttempts(), 9ull, "pool: a publicacao reconta");
        CheckEq(static_cast<u32>(G.Layout.Layout().Regions().size()), 4u, "com um pool so");
        CheckEq(G.Cap(0), 6u, "e o opaco da malha B entra no pool A");
    }
    {
        // IndexCount que nao descreve triangulos inteiros. O numero de pools NAO muda aqui, entao so a
        // versao da tabela pode disparar as duas contagens.
        G.MalhaA.InitFromPool(G.PoolA, 0, 32 * 1024, 48 * 1024, 3, 4, 1);
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 10ull, "o Setup da malha, antes de publicar, ja reconta");
        CheckEq(G.Cap(0), 0u, "sobre as linhas de malha vazias, nenhuma instancia conta");
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 11ull, "e a publicacao das linhas novas reconta de novo");
        CheckEq(G.Cap(0), 1u, "mascara de malha inutilizavel: so o opaco da malha B conta");
    }
    {
        // A chave da malha e GROSSA de proposito: um Setup que republica as mesmas linhas tambem reconta.
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 12ull, "um Setup de malha sem mudanca tambem reconta");
    }
    {
        FMaterial* Lista[3]{ &G.Opaco, &G.Mascarado, &G.Troca };
        G.Materials.SetupForMaterials(G.Device.Get(), G.Heap, Lista);
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 13ull, "o Setup do material, antes de publicar, ja reconta");
        CheckEq(G.Cap(0), 0u, "sobre as linhas de material vazias, nenhuma instancia conta");
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 14ull, "e a publicacao reconta de novo");
        CheckEq(G.Cap(0), 1u, "com a contagem de antes");
    }
    {
        G.Instances.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 15ull, "o Setup das instancias, antes de publicar, ja reconta");
        CheckEq(G.Cap(0), 0u, "sobre a meta vazia, nenhuma instancia conta");
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 16ull, "e a publicacao reconta de novo");
        CheckEq(G.Cap(0), 1u, "com a contagem de antes");
    }

    // --- dois frames em voo ---------------------------------------------------------------------
    //
    // Gravados os dois antes de qualquer um executar, e a MESMA linha muda nos dois. Se os uploads
    // dividissem o staging, a copia do primeiro leria o valor do segundo, e a leitura do primeiro frame
    // mostraria a cor errada.
    {
        const u32 A = G.ProximoSlot;
        const u32 B = (A + 1u) % FGpuTableStorage::kFrameSlots;
        G.Opaco.Constants.BaseColorFactor = { 0.9f, 0.1f, 0.1f, 1.0f };
        G.Materials.MarkDirty();
        G.Gravar(A);
        CheckEq(G.Materials.LastDirtyRows(), 1u, "em voo: o primeiro frame publica a cor");
        G.Opaco.Constants.BaseColorFactor = { 0.1f, 0.9f, 0.1f, 1.0f };
        G.Materials.MarkDirty();
        G.Gravar(B);
        CheckEq(G.Materials.LastDirtyRows(), 1u, "em voo: o segundo frame publica outra cor na mesma linha");
        G.Executar(A);
        G.Executar(B);
    }

    for (u32 I = 0; I < 4; ++I) G.Frame();
    CheckEq64(G.Layout.RebuildAttempts(), 16ull, "depois dos eventos, a cena assentada nao reconta");
    CheckEq(G.Motivo, GpuLayoutRefreshOk, "e o layout continua valido");

    // --- Release: as linhas publicadas somem, e a versao acompanha ------------------------------
    G.Materials.Release(G.Heap);
    G.Consultar();
    CheckEq64(G.Layout.RebuildAttempts(), 17ull, "o Release do material reconta");
    CheckEq(G.Cap(0), 0u, "sem material publicado, nenhuma instancia conta");
    G.Instances.Release(G.Heap);
    G.Consultar();
    CheckEq64(G.Layout.RebuildAttempts(), 18ull, "o Release das instancias reconta");

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
} catch (const std::exception& E) {
    std::cerr << "Smile.GpuIndirectLayout: excecao -- " << E.what() << '\n';
    return 2;
}
