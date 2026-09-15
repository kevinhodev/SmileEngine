// QUANDO o layout de regioes e refeito, e o que as tabelas PUBLICAM, sobre tabelas, cena e fila de
// verdade (Fase 5 do GPU-DRIVEN-PLAN).
//
// Tres contratos:
//
//  1. O layout so e recontado quando as linhas publicadas mudam o que a capacidade conta — ocupacao,
//     associacao, classe, pool, `RaytracingOnly` e mascara de malha inutilizavel. Transform,
//     assentamento, visibilidade, mobilidade e cor nao o refazem. O `TestarChavesDoLayout`
//     (Smile.GpuClassifyWarp) prova que a chave e completa bit a bit; este prova que as tabelas de
//     PRODUCAO a consultam, pelos caminhos que o editor usa. Todo evento que NAO deve recontar confere
//     que a tabela publicou alguma linha: sem isso, "nao recontou" passaria por nada ter acontecido.
//
//  2. "Publicado" e o que a GPU tem, e o frame publica as tres tabelas ou nao existe. Cada frame
//     segue a ordem do RendererFrame: produtor e `GpuScenePublicationGate` ANTES de abrir a lista, e,
//     recusada a publicacao, nao ha frame — nenhuma tabela grava e nenhum leitor roda. Aceita, o frame
//     e gravado, EXECUTADO e esperado pela fence, e as tres tabelas — as quatro colunas das instancias
//     — sao lidas de volta no fim da propria lista, onde um leitor do frame estaria, e comparadas byte
//     a byte com as linhas publicadas no momento da gravacao. A gravacao de cada tabela continua
//     transacional sozinha, e isso e conferido isolado: recusada (lista nula), devolve false e nao
//     avanca nem as linhas publicadas nem a versao, e a nova tentativa publica tudo. Dois frames
//     gravados antes de qualquer um executar provam que cada upload le o staging do SEU frame slot.
//
//  3. O par temporal publicado segue a GPU, e nao as linhas escritas. A leitura de volta so prova que
//     a GPU tem o que as linhas publicadas dizem; o par CERTO e conferido contra matrizes calculadas
//     aqui. O previous e o current que a GPU tinha: A→B recusado publica B/A na nova tentativa, A→B
//     recusado e depois C publica C/A, e so o frame seguinte a publicacao assenta. Um objeto nascido
//     numa recusa nao tem passado na GPU. `TransformsFor` (o ObjectCB) e `MetaFor` respondem pela
//     mesma imagem.
//
// A camada de debug participa: ERROR e CORRUPTION reprovam.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
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

    // Um frame cuja lista e NULA: a recusa que a porta do armazenamento aplica, vista pela fronteira do
    // frame antes de qualquer tabela gravar.
    constexpr bool kListaRecusada = true;

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
        FLeitura                               Leituras[FGpuTableStorage::kFrameSlots][6];
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
        u32 Publicacao = GpuScenePublicationOk; // a decisao da fronteira no ultimo frame

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
                // Caixa finita: a default, de ±1e9, absorve em float qualquer translacao pequena, e a
                // caixa publicada nao andaria com o objeto.
                R.LocalAABBMin = { -1.0f, -1.0f, -1.0f };
                R.LocalAABBMax = { 1.0f, 1.0f, 1.0f };
                R.RefreshWorldBounds();
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

        // Grava um frame no slot, na ordem do RendererFrame. O produtor e a porta da publicacao vem
        // ANTES de abrir a lista; recusada, nao ha frame, e devolve false sem gravar nada. Aceita, os
        // tres uploads, o layout, e a leitura das tres tabelas — as quatro colunas das instancias — no
        // FIM da lista, com a imagem esperada tirada das linhas publicadas neste instante.
        bool Gravar(u32 _Slot, bool _ListaRecusada = false) {
            ID3D12GraphicsCommandList* L = _ListaRecusada ? nullptr : List[_Slot].Get();
            Instances.PrepareFrame(Scene, Meshes, Materials);
            Publicacao = GpuScenePublicationGate(Materials, Meshes, Instances, L);
            if (Publicacao != GpuScenePublicationOk) return false;
            if (!L) {
                Check(false, "a porta aceitou um frame sem lista");
                return false;
            }

            Hr(Allocator[_Slot]->Reset(), "Reset do allocator");
            Hr(L->Reset(Allocator[_Slot].Get(), nullptr), "Reset da lista");
            const bool Gravou = Materials.RecordUpload(L, _Slot) && Meshes.RecordUpload(L, _Slot) &&
                                Instances.RecordUpload(L, _Slot);
            Check(Gravou, "a porta aceitou a publicacao, e as tres gravacoes tambem");
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
            Ler(Leituras[_Slot][1], "bounds", Instances.BoundsResource(),
                ComoBytes(Instances.PublishedBounds()));
            Ler(Leituras[_Slot][2], "transform", Instances.TransformResource(),
                ComoBytes(Instances.PublishedTransforms()));
            Ler(Leituras[_Slot][3], "prev transform", Instances.PrevTransformResource(),
                ComoBytes(Instances.PublishedPrevTransforms()));
            Ler(Leituras[_Slot][4], "material", Materials.Resource(),
                ComoBytes(Materials.PublishedRows()));
            Ler(Leituras[_Slot][5], "malha", Meshes.Resource(), ComoBytes(Meshes.PublishedRows()));

            const HRESULT Fechou = L->Close();
            if (FAILED(Fechou)) Drenar("gravacao");
            Hr(Fechou, "Close");
            return true;
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

        // Um frame, gravado e executado antes do seguinte. Sem frame, o slot nao anda — o renderer
        // aborta antes do BeginFrame, que e quem o avanca.
        void Frame(bool _ListaRecusada = false) {
            const u32 Slot = ProximoSlot;
            if (!Gravar(Slot, _ListaRecusada)) return;
            ProximoSlot = (ProximoSlot + 1u) % FGpuTableStorage::kFrameSlots;
            Executar(Slot);
        }

        FRenderable& R(u64 _Id) { return *Scene.FindRenderable(_Id); }
        u32 Indice(u64 _Id) const { return static_cast<u32>(Scene.IndexOfRenderable(_Id)); }

        // Move o objeto em X pelo caminho do editor e devolve o transform novo.
        Mat44 Mover(u64 _Id, f32 _Dx) {
            FRenderable& Obj = R(_Id);
            Obj.Transform.Position.X += _Dx;
            Obj.RefreshWorldBounds();
            Scene.BumpTransformsVersion(Indice(_Id));
            return Obj.Transform.Matrix();
        }

        // O par temporal PUBLICADO do objeto, contra matrizes calculadas pelo teste. A leitura de volta
        // do frame ja provou que a GPU tem exatamente as colunas publicadas; as duas conferencias juntas
        // amarram a GPU ao par esperado. O flag de movimento acompanha o par, e `TransformsFor` (o
        // ObjectCB) e `MetaFor` respondem pela mesma imagem.
        void ConferirPar(u64 _Id, const Mat44& _Current, const Mat44& _Previous,
                         std::string_view _Caso) {
            const std::string     Caso(_Caso);
            const FGpuSceneHandle H        = R(_Id).GpuHandle;
            const auto            Meta     = Instances.PublishedMeta();
            const auto            Atual    = Instances.PublishedTransforms();
            const auto            Anterior = Instances.PublishedPrevTransforms();
            if (H.Slot >= Meta.size() || H.Slot >= Atual.size() || H.Slot >= Anterior.size()) {
                Check(false, Caso + ": o slot esta publicado");
                return;
            }
            const FGpuSceneInstanceTransform Cur{ _Current }, Prev{ _Previous };
            CheckEq(Meta[H.Slot].Generation, H.Generation, Caso + ": a linha publicada e deste objeto");
            Check(Atual[H.Slot] == Cur, Caso + ": current publicado");
            Check(Anterior[H.Slot] == Prev, Caso + ": previous publicado");
            Check(((Meta[H.Slot].Flags & GpuSceneInstanceTransformChanged) != 0u) == (Cur != Prev),
                  Caso + ": o flag de movimento acompanha o par");

            Mat44 C{}, P{};
            Check(Instances.TransformsFor(H, C, P) && FGpuSceneInstanceTransform{ C } == Cur &&
                      FGpuSceneInstanceTransform{ P } == Prev,
                  Caso + ": TransformsFor devolve o par publicado");
            FGpuSceneInstanceMeta M{};
            Check(Instances.MetaFor(H, M) && M == Meta[H.Slot],
                  Caso + ": MetaFor devolve a meta publicada");
        }

        // O centro X publicado da caixa do objeto, ou NaN quando o slot nao esta publicado.
        f32 CentroPublicado(u64 _Id) const {
            const FGpuSceneHandle H      = Scene.FindRenderable(_Id)->GpuHandle;
            const auto            Bounds = Instances.PublishedBounds();
            return H.Slot < Bounds.size() ? Bounds[H.Slot].Center.X
                                          : std::numeric_limits<f32>::quiet_NaN();
        }

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

        // A porta por NUMERO de linhas, a que as portas pendentes das tabelas usam. Nos limites: o
        // buffer cheio cabe, uma linha a mais nao.
        Check(S.AcceptsRows(nullptr, 0u), "porta das linhas: nenhuma linha e aceito, ate sem lista");
        Check(S.AcceptsRows(L.Get(), S.Capacity()), "porta das linhas: o buffer cheio cabe");
        Check(!S.AcceptsRows(L.Get(), S.Capacity() + 1u),
              "porta das linhas: uma linha alem do buffer recusa");
        Check(!S.AcceptsRows(nullptr, 1u), "porta das linhas: lista nula recusa");
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

    // Sem publicacao pendente, as portas aceitam ate sem lista: o frame so e recusado por uma copia que
    // de fato precisaria acontecer.
    Check(G.Materials.AcceptsPendingUpload(nullptr),
          "sem publicacao pendente: a porta do material aceita ate sem lista");
    Check(G.Meshes.AcceptsPendingUpload(nullptr),
          "sem publicacao pendente: a porta da malha aceita ate sem lista");
    Check(G.Instances.AcceptsPendingUpload(nullptr),
          "sem publicacao pendente: a porta das instancias aceita ate sem lista");

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

    // --- classe, com a publicacao RECUSADA primeiro ---------------------------------------------
    //
    // Duas camadas. A gravacao da tabela, sozinha, e transacional: recusada, devolve false e nada anda.
    // E a fronteira do frame nem chega a grava-la: com a lista recusada, nao ha frame.
    {
        const u32 LinhaTroca = 3u; // linha 0 e o default; Opaco, Mascarado, Troca
        G.Troca.TwoSided = true;
        G.Materials.MarkDirty();
        Check(!G.Materials.AcceptsPendingUpload(nullptr), "classe: a porta pendente recusa sem lista");
        Check(!G.Materials.RecordUpload(nullptr, 0u), "classe: a gravacao recusada devolve false");
        CheckEq(G.Materials.LastDirtyRows(), 0u, "classe recusada: nenhuma linha publicada");
        CheckEq(G.Materials.PublishedRows()[LinhaTroca].PipelineClass,
                static_cast<u32>(GpuPipelineClassOpaqueOneSided),
                "classe recusada: a linha publicada continua a antiga");
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 1ull, "classe recusada: a versao nao sobe e nada reconta");
        CheckEq(G.Cap(1), 3u, "classe recusada: a capacidade continua a da GPU");

        G.Frame(kListaRecusada);
        CheckEq(G.Publicacao, GpuScenePublicationRefusedMaterials,
                "classe: a fronteira recusa o frame pelo material");
        CheckEq(G.Materials.PublishedRows()[LinhaTroca].PipelineClass,
                static_cast<u32>(GpuPipelineClassOpaqueOneSided),
                "classe, sem frame: a linha publicada continua a antiga");

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

    // --- RaytracingOnly, com a publicacao das instancias RECUSADA e ciclos novos do produtor ------
    {
        const u32 Slot = G.R(G.IdTroca[0]).GpuHandle.Slot;
        G.R(G.IdTroca[0]).RaytracingOnly = true;
        G.Scene.BumpStaticCastersVersion(G.Indice(G.IdTroca[0]));
        G.Instances.PrepareFrame(G.Scene, G.Meshes, G.Materials);
        Check(!G.Instances.RecordUpload(nullptr, 0u), "instancias: a gravacao recusada devolve false");
        CheckEq(G.Instances.LastDirtyRows(), 0u, "instancias recusadas: nenhuma linha publicada");
        Check((G.Instances.PublishedMeta()[Slot].Flags & GpuSceneInstanceRaytracingOnly) == 0u,
              "instancias recusadas: a linha publicada continua a antiga");
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 3ull, "instancias recusadas: nada reconta");

        // Outro objeto se move ANTES da nova tentativa, e o frame dele tambem e recusado, agora pela
        // fronteira: dois ciclos novos do produtor, e o slot pendente nao pode ser esquecido em nenhum.
        FRenderable& Outro = G.R(G.IdOpaco[2]);
        Outro.Transform.Position.Z += 1.0f;
        Outro.RefreshWorldBounds();
        G.Scene.BumpTransformsVersion(G.Indice(G.IdOpaco[2]));
        G.Frame(kListaRecusada);
        CheckEq(G.Publicacao, GpuScenePublicationRefusedInstances,
                "RaytracingOnly: a fronteira recusa o frame pelas instancias");
        G.Frame();
        Check((G.Instances.PublishedMeta()[Slot].Flags & GpuSceneInstanceRaytracingOnly) != 0u,
              "RaytracingOnly: a nova tentativa publica a linha pendente");
        CheckEq64(G.Layout.RebuildAttempts(), 4ull, "RaytracingOnly reconta");
        CheckEq(G.Cap(1), 2u, "e a linha sai da contagem");
    }

    // --- o par temporal, com o frame RECUSADO ---------------------------------------------------
    //
    // O previous publicado e o current que a GPU tinha. Um frame recusado deixa a GPU no par antigo:
    // a nova tentativa publica o movimento inteiro a partir do que a GPU tem — e nao do que o produtor
    // escreveu e nunca subiu —, e so o frame SEGUINTE a publicacao assenta.
    {
        const u64 Id = G.IdOpaco[0];
        for (u32 I = 0; I < 2; ++I) G.Frame(); // assenta o objeto que o bloco anterior moveu
        {
            const Mat44 A = G.R(Id).Transform.Matrix();
            G.ConferirPar(Id, A, A, "A→B: parte assentado em A");
            const f32 CentroA = G.CentroPublicado(Id);

            const Mat44 B       = G.Mover(Id, 2.0f);
            const f32   CentroB = (G.R(Id).AABBMin.X + G.R(Id).AABBMax.X) * 0.5f;
            Check(CentroB != CentroA, "A→B: a caixa anda com o objeto");
            G.Frame(kListaRecusada);
            CheckEq(G.Publicacao, GpuScenePublicationRefusedInstances,
                    "A→B recusado: a fronteira recusa o frame pelas instancias");
            G.ConferirPar(Id, A, A, "A→B recusado: a GPU continua em A/A");
            Check(G.CentroPublicado(Id) == CentroA, "A→B recusado: a caixa publicada continua a antiga");

            G.Frame();
            G.ConferirPar(Id, B, A, "A→B recusado, nova tentativa: Current=B, Previous=A");
            Check(G.CentroPublicado(Id) == CentroB, "A→B, nova tentativa: a caixa publicada e a nova");
            G.Frame();
            G.ConferirPar(Id, B, B, "A→B recusado: so o frame seguinte assenta B/B");
        }
        {
            const Mat44 A = G.R(Id).Transform.Matrix();
            G.Mover(Id, 2.0f);
            G.Frame(kListaRecusada);
            CheckEq(G.Publicacao, GpuScenePublicationRefusedInstances,
                    "A→B recusado e depois C: a fronteira recusa o frame pelas instancias");
            G.ConferirPar(Id, A, A, "A→B recusado e depois C: a recusa deixa A/A");
            const Mat44 C = G.Mover(Id, 2.0f);
            G.Frame();
            G.ConferirPar(Id, C, A, "A→B recusado e depois C: Current=C, Previous=A");
            G.Frame();
            G.ConferirPar(Id, C, C, "A→B recusado e depois C: so o frame seguinte assenta C/C");
        }
        CheckEq64(G.Layout.RebuildAttempts(), 4ull, "o par temporal nao reconta");
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
        // Acrescentado num frame RECUSADO, e movido antes da nova tentativa. Ele nunca esteve na GPU,
        // entao nao tem passado la: a nova tentativa publica previous = current — nem o transform com
        // que nasceu, nem o que o shadow guarda naquele slot, que nao e dele.
        FRenderable Novo;
        Novo.Mesh     = &G.MalhaA;
        Novo.Material = &G.Opaco;
        const u64             IdNovo = G.Scene.AddRenderable(Novo).Id;
        const FGpuSceneHandle H      = G.R(IdNovo).GpuHandle;
        G.Frame(kListaRecusada);
        CheckEq(G.Publicacao, GpuScenePublicationRefusedInstances,
                "acrescentar: a fronteira recusa o frame pelas instancias");
        CheckEq64(G.Layout.RebuildAttempts(), 6ull, "acrescentar recusado: nada reconta");
        Mat44                 Cur{}, Prev{};
        FGpuSceneInstanceMeta Meta{};
        Check(!G.Instances.TransformsFor(H, Cur, Prev),
              "acrescentar recusado: o novo nao tem par publicado");
        Check(!G.Instances.MetaFor(H, Meta), "acrescentar recusado: nem meta publicada");

        const Mat44 Movido = G.Mover(IdNovo, 5.0f);
        G.Frame();
        CheckEq64(G.Layout.RebuildAttempts(), 7ull, "ocupacao: acrescentar reconta");
        CheckEq(G.Cap(0), 5u, "e o novo entra no pool A");
        G.ConferirPar(IdNovo, Movido, Movido, "nascido numa recusa e movido: sem passado na GPU");
    }

    // --- pool, com a publicacao da malha RECUSADA -----------------------------------------------
    {
        G.MalhaB.InitFromPool(G.PoolA, 3200, 40 * 1024, 56 * 1024, 3, 3, 1);
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        Check(!G.Meshes.RecordUpload(nullptr, 0u), "malha: a gravacao recusada devolve false");
        CheckEq(G.Meshes.LastDirtyRows(), 0u, "malha recusada: nenhuma linha publicada");
        Check(G.Meshes.PublishedRows().empty(), "malha recusada: as linhas publicadas seguem vazias");
        G.Consultar();
        CheckEq64(G.Layout.RebuildAttempts(), 8ull, "o Setup reconta, e a recusa nao acrescenta nada");
        CheckEq(G.Cap(0), 0u, "sobre as linhas de malha vazias, nenhuma instancia conta");

        G.Frame(kListaRecusada);
        CheckEq(G.Publicacao, GpuScenePublicationRefusedMeshes,
                "pool: a fronteira recusa o frame pela malha");
        Check(G.Meshes.PublishedRows().empty(), "pool, sem frame: as linhas publicadas seguem vazias");

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
        Check(G.Gravar(A), "em voo: o primeiro frame existe");
        CheckEq(G.Materials.LastDirtyRows(), 1u, "em voo: o primeiro frame publica a cor");
        G.Opaco.Constants.BaseColorFactor = { 0.1f, 0.9f, 0.1f, 1.0f };
        G.Materials.MarkDirty();
        Check(G.Gravar(B), "em voo: o segundo frame existe");
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
