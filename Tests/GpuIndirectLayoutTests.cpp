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
//     segue a ordem do RendererFrame: produtor e `GpuScenePreparePublication` ANTES de abrir a lista,
//     e, recusada a publicacao, nao ha frame — nenhuma tabela grava e nenhum leitor roda. Aceita, a
//     lista abre e `GpuSceneRecordPublication` grava as tres SEM ALOCAR — o teste conta as alocacoes
//     da thread durante a gravacao, com o operator new global substituido. O frame e EXECUTADO e
//     esperado pela fence, e as tres tabelas — as quatro colunas das instancias — sao lidas de volta
//     no fim da propria lista, onde um leitor do frame estaria, e comparadas byte a byte com as linhas
//     publicadas no momento da gravacao. A preparacao de cada tabela continua transacional sozinha, e
//     isso e conferido isolado: recusada (lista nula), nao avanca nem as linhas publicadas nem a
//     versao, e a nova tentativa publica tudo. Uma preparacao invalidada — mutacao da tabela, outra
//     lista — nao grava nada. Dois frames gravados antes de qualquer um executar provam que cada
//     upload le o staging do SEU frame slot.
//     E um Setup INTERROMPIDO — o failpoint de upload recusando o staging, em cada tabela e entre as
//     tres — deixa as tabelas liberadas, sem mapa nem descritor preso, e a porta recusa o frame ate
//     um Setup completo.
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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Debug/VramTracker.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Scene/GpuIndirectLayoutState.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"
#include "Smile/Scene/Scene.h"

using namespace Smile;
namespace wrl = Microsoft::WRL;

// === Contagem de alocacoes =========================================================================
//
// A fase 2 da publicacao roda com a lista do frame aberta, e uma excecao ali deixaria shadows
// avancados pela metade. "Nao aloca" dito so em comentario nao se sustenta, entao o executavel troca o
// operator new global e conta as alocacoes da thread enquanto a contagem esta ligada. O D3D12 aloca
// pelos proprios heaps, fora deste operator: o que se conta e o codigo da engine.
namespace {
    thread_local bool GContarAlocacoes = false;
    thread_local u64  GAlocacoes       = 0;
    // Falta de memoria simulada: toda alocacao da thread lanca bad_alloc enquanto ligada.
    thread_local bool GFalharAlocacoes = false;

    void* Alocar(std::size_t _Bytes) {
        if (GContarAlocacoes) ++GAlocacoes;
        if (GFalharAlocacoes) throw std::bad_alloc();
        if (void* P = std::malloc(_Bytes != 0 ? _Bytes : 1)) return P;
        throw std::bad_alloc();
    }

    void* AlocarAlinhado(std::size_t _Bytes, std::align_val_t _Alinhamento) {
        if (GContarAlocacoes) ++GAlocacoes;
        if (GFalharAlocacoes) throw std::bad_alloc();
        if (void* P = _aligned_malloc(_Bytes != 0 ? _Bytes : 1,
                                      static_cast<std::size_t>(_Alinhamento)))
            return P;
        throw std::bad_alloc();
    }

    // Durante a vida do escopo, toda alocacao desta thread falha.
    struct FFaltaDeMemoria {
        FFaltaDeMemoria() { GFalharAlocacoes = true; }
        ~FFaltaDeMemoria() { GFalharAlocacoes = false; }
    };

    // Conta as alocacoes desta thread entre a construcao e o `Total`.
    struct FContagemDeAlocacoes {
        FContagemDeAlocacoes() {
            GAlocacoes       = 0;
            GContarAlocacoes = true;
        }
        ~FContagemDeAlocacoes() { GContarAlocacoes = false; }
        u64 Total() {
            GContarAlocacoes = false;
            return GAlocacoes;
        }
    };
}

void* operator new(std::size_t _Bytes) { return Alocar(_Bytes); }
void* operator new[](std::size_t _Bytes) { return Alocar(_Bytes); }
void* operator new(std::size_t _Bytes, const std::nothrow_t&) noexcept {
    try { return Alocar(_Bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t _Bytes, const std::nothrow_t&) noexcept {
    try { return Alocar(_Bytes); } catch (...) { return nullptr; }
}
void operator delete(void* _P) noexcept { std::free(_P); }
void operator delete[](void* _P) noexcept { std::free(_P); }
void operator delete(void* _P, std::size_t) noexcept { std::free(_P); }
void operator delete[](void* _P, std::size_t) noexcept { std::free(_P); }
void operator delete(void* _P, const std::nothrow_t&) noexcept { std::free(_P); }
void operator delete[](void* _P, const std::nothrow_t&) noexcept { std::free(_P); }
void* operator new(std::size_t _Bytes, std::align_val_t _A) { return AlocarAlinhado(_Bytes, _A); }
void* operator new[](std::size_t _Bytes, std::align_val_t _A) { return AlocarAlinhado(_Bytes, _A); }
void operator delete(void* _P, std::align_val_t) noexcept { _aligned_free(_P); }
void operator delete[](void* _P, std::align_val_t) noexcept { _aligned_free(_P); }
void operator delete(void* _P, std::size_t, std::align_val_t) noexcept { _aligned_free(_P); }
void operator delete[](void* _P, std::size_t, std::align_val_t) noexcept { _aligned_free(_P); }

// E a fase 2 nao LANCA, pela assinatura: sem noexcept, uma alocacao que voltasse para ali seria uma
// excecao possivel com a lista aberta, e nao um terminate.
static_assert(noexcept(std::declval<FGpuMaterialTable&>().RecordUpload(nullptr, 0u)),
              "a gravacao da tabela de materiais e noexcept");
static_assert(noexcept(std::declval<FGpuMeshTable&>().RecordUpload(nullptr, 0u)),
              "a gravacao da tabela de malhas e noexcept");
static_assert(noexcept(std::declval<FGpuSceneInstanceTable&>().RecordUpload(nullptr, 0u)),
              "a gravacao da tabela de instancias e noexcept");
static_assert(noexcept(GpuSceneRecordPublication(std::declval<FGpuMaterialTable&>(),
                                                 std::declval<FGpuMeshTable&>(),
                                                 std::declval<FGpuSceneInstanceTable&>(),
                                                 nullptr, 0u)),
              "a gravacao da publicacao do frame e noexcept");
// A recontagem do layout le as linhas que a gravacao acabou de publicar, com a mesma lista aberta.
static_assert(noexcept(std::declval<FGpuIndirectLayoutState&>().Refresh(
                  std::declval<const FGpuLayoutInputs&>())),
              "recontar o layout e noexcept");

// E o ROLLBACK tambem nao lanca: uma excecao no meio de uma liberacao a interromperia, com range
// vazado, buffers presos e as tabelas seguintes intactas.
static_assert(noexcept(std::declval<FTextureSRVHeap&>().Free(0u, 1u)),
              "devolver slot ao heap e noexcept");
static_assert(noexcept(std::declval<FGpuTableStorage&>().Release(std::declval<FTextureSRVHeap&>())),
              "liberar o armazenamento e noexcept");
static_assert(noexcept(GpuSceneReleaseTables(std::declval<FGpuMaterialTable&>(),
                                             std::declval<FGpuMeshTable&>(),
                                             std::declval<FGpuSceneInstanceTable&>(),
                                             std::declval<FTextureSRVHeap&>())),
              "liberar as tres tabelas e noexcept");

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
        // Consultas que RECONTARAM. Sem este numero, "recontar nao aloca" passaria verde num teste em
        // que nenhuma consulta chegou a recontar.
        u32 Remontagens = 0;
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
        //
        // A recontagem roda com a lista aberta, entao nao pode alocar: a reserva vem antes, e a contagem
        // de alocacoes cobre a consulta inteira — inclusive os frames em que as versoes mudaram e o
        // layout foi remontado.
        void Consultar() {
            const u32 Pools = static_cast<u32>(Meshes.GeometryPools().size());
            Layout.Reserve(Pools);
            const FGpuLayoutInputs In{
                Instances.PublishedMeta(), Materials.PublishedRows(), Meshes.PublishedRows(), Pools,
                Instances.LayoutInputsVersion(), Materials.LayoutInputsVersion(),
                Meshes.LayoutInputsVersion()
            };
            const u64 TentativasAntes = Layout.RebuildAttempts();
            u64 Alocacoes = 0;
            {
                FContagemDeAlocacoes Contagem;
                Motivo    = Layout.Refresh(In);
                Alocacoes = Contagem.Total();
            }
            CheckEq64(Alocacoes, 0ull, "recontar o layout nao aloca");
            if (Layout.RebuildAttempts() != TentativasAntes) ++Remontagens;
        }

        // Grava um frame no slot, na ordem do RendererFrame. O produtor e a preparacao da publicacao
        // vem ANTES de abrir a lista; recusada, nao ha frame, e devolve false sem gravar nada. Aceita, a
        // gravacao das tres — contada, porque nao pode alocar —, o layout, e a leitura das tres tabelas
        // — as quatro colunas das instancias — no FIM da lista, com a imagem esperada tirada das linhas
        // publicadas neste instante.
        bool Gravar(u32 _Slot, bool _ListaRecusada = false) {
            ID3D12GraphicsCommandList* L = _ListaRecusada ? nullptr : List[_Slot].Get();
            Instances.PrepareFrame(Scene, Meshes, Materials);
            Publicacao = GpuScenePreparePublication(Materials, Meshes, Instances, L);
            if (Publicacao != GpuScenePublicationOk) return false;
            if (!L) {
                Check(false, "a porta aceitou um frame sem lista");
                return false;
            }

            Hr(Allocator[_Slot]->Reset(), "Reset do allocator");
            Hr(L->Reset(Allocator[_Slot].Get(), nullptr), "Reset da lista");
            bool Gravou    = false;
            u64  Alocacoes = 0;
            {
                FContagemDeAlocacoes Contagem;
                Gravou    = GpuSceneRecordPublication(Materials, Meshes, Instances, L, _Slot);
                Alocacoes = Contagem.Total();
            }
            Check(Gravou, "a publicacao preparada e gravada");
            CheckEq64(Alocacoes, 0ull, "gravar a publicacao nao aloca");
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
        L->Close();
        S.Release(G.Heap);
        G.Drenar("porta");
    }

    // --- o failpoint de upload: deixa passar N criacoes e falha as seguintes --------------------
    //
    // Os testes de Setup abaixo dependem dele para falhar uma coluna que NAO e a primeira.
    {
        auto Criar = [&]() {
            return GpuResources::TryCreateUploadBuffer(G.Device.Get(), 64u, 1u, false);
        };
        GpuResources::FUploadFailureInjection Falha(1u, 1u);
        const GpuResources::FUploadBuffer A = Criar();
        const GpuResources::FUploadBuffer B = Criar();
        CheckEq(GpuResources::PendingUploadFailures(), 0u, "failpoint: a falha foi consumida");
        const GpuResources::FUploadBuffer C = Criar();
        Check(static_cast<bool>(A), "failpoint: a criacao pulada passa");
        Check(!B, "failpoint: a criacao seguinte falha");
        Check(static_cast<bool>(C), "failpoint: esgotada a injecao, as criacoes voltam a passar");
    }

    // --- o Create do armazenamento: o antigo sai ANTES do novo, e a falha sai liberada -----------
    //
    // O staging e criado DEPOIS do buffer, entao recusa-lo e a falha em que ja existe um recurso novo
    // para desfazer. Novo ou crescendo, o armazenamento sai liberado. E o pico do crescimento e so o
    // novo: o failpoint registra a VRAM rastreada no instante em que o staging novo e pedido, e o
    // buffer antigo ja nao pode estar nela.
    {
        wrl::ComPtr<ID3D12CommandAllocator>    Alloc;
        wrl::ComPtr<ID3D12GraphicsCommandList> L;
        Hr(G.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Alloc)),
           "CreateCommandAllocator (atomico)");
        Hr(G.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Alloc.Get(), nullptr,
                                       IID_PPV_ARGS(&L)),
           "CreateCommandList (atomico)");
        auto Recusado = [&](FGpuTableStorage& _S, u32 _Capacidade) {
            GpuResources::FUploadFailureInjection Falha(1u);
            bool Lancou = false;
            try {
                _S.Create(G.Device.Get(), G.Heap, sizeof(u32), _Capacidade, "atomico");
            } catch (const std::exception&) {
                Lancou = true;
            }
            CheckEq(GpuResources::PendingUploadFailures(), 0u, "Create: a falha foi consumida pelo staging");
            return Lancou;
        };

        // O tamanho que o VramTracker cobra por um buffer — o mesmo GetResourceAllocationInfo dele.
        auto BytesDoBuffer = [&](u64 _Largura) {
            D3D12_RESOURCE_DESC D{};
            D.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            D.Width            = _Largura;
            D.Height           = 1;
            D.DepthOrArraySize = 1;
            D.MipLevels        = 1;
            D.SampleDesc.Count = 1;
            D.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return G.Device->GetResourceAllocationInfo(0, 1, &D).SizeInBytes;
        };
        // A largura do recurso vivo, e nao o ponteiro: com o antigo solto antes da criacao, o novo
        // pode nascer no mesmo endereco.
        auto Largura = [&](const FGpuTableStorage& _S) {
            return _S.Resource() ? _S.Resource()->GetDesc().Width : 0ull;
        };

        const u32 Usados = G.Heap.UsedSlots();
        FGpuTableStorage S;
        Check(Recusado(S, 4u), "Create novo: o staging recusado lanca");
        Check(!S.IsBuilt(), "Create novo recusado: o armazenamento continua sem recurso");
        CheckEq(S.Capacity(), 0u, "Create novo recusado: e sem capacidade");
        CheckEq(S.Srv(), FTextureSRVHeap::kInvalidSlot, "Create novo recusado: e sem descritor");
        CheckEq(G.Heap.UsedSlots(), Usados, "Create novo recusado: nenhum descritor vazou");

        // Crescer com sucesso: recurso novo, capacidade nova, o MESMO slot de descritor.
        S.Create(G.Device.Get(), G.Heap, sizeof(u32), 64u, "atomico");
        const u32 Slot = S.Srv();
        S.Create(G.Device.Get(), G.Heap, sizeof(u32), 32768u, "atomico");
        CheckEq(S.Capacity(), 32768u, "crescer: a capacidade nova");
        CheckEq64(Largura(S), 32768ull * sizeof(u32), "crescer: o buffer vivo tem o tamanho novo");
        CheckEq(S.Srv(), Slot, "crescer: o slot do descritor e preservado");

        // Crescer RECUSADO, medindo o pico. Os dois tamanhos sao maiores que o alinhamento de 64 KB, e
        // diferentes, para a conta separar o buffer antigo do novo.
        const u64 BufferAtual = BytesDoBuffer(32768ull * sizeof(u32));
        const u64 BufferNovo  = BytesDoBuffer(65536ull * sizeof(u32));
        Check(BufferNovo > BufferAtual, "o buffer novo e maior que o atual");
        const u64 VramAntes = VramTracker::Snapshot().TotalTracked;
        Check(Recusado(S, 65536u), "crescer: o staging recusado lanca");
        CheckEq64(GpuResources::TrackedVramAtLastInjectedFailure(),
                  VramAntes - BufferAtual + BufferNovo,
                  "crescer: quando o staging novo e pedido, o buffer antigo ja saiu — o pico e so o "
                  "novo");
        Check(!S.IsBuilt(), "crescer recusado: o armazenamento sai liberado");
        CheckEq(S.Capacity(), 0u, "crescer recusado: sem capacidade");
        CheckEq(S.Srv(), FTextureSRVHeap::kInvalidSlot, "crescer recusado: sem descritor");
        CheckEq(G.Heap.UsedSlots(), Usados, "crescer recusado: o descritor voltou ao heap");
        CheckEq64(VramTracker::Snapshot().TotalTracked, VramAntes - BufferAtual,
                  "crescer recusado: nem o buffer antigo nem o novo ficaram vivos");

        // Stride diferente com a MESMA capacidade tambem recria: o buffer foi dimensionado pelo stride
        // antigo, e manter os dois descreveria linhas que nao cabem nele.
        S.Create(G.Device.Get(), G.Heap, sizeof(u32), 64u, "atomico");
        S.Create(G.Device.Get(), G.Heap, 2u * sizeof(u32), 64u, "atomico");
        CheckEq64(Largura(S), 64ull * 2u * sizeof(u32), "stride diferente: o buffer e recriado");
        CheckEq(S.RowBytes(), 2u * sizeof(u32), "stride diferente: o stride novo vale");
        L->Close();
        S.Release(G.Heap);
        CheckEq(G.Heap.UsedSlots(), Usados, "o Release devolve o descritor");
        G.Drenar("atomico");
    }

    // --- o heap devolve slots SEM ALOCAR, ate com a free list no maximo -------------------------
    //
    // O rollback de falta de memoria devolve descritores, e o Free era a peca que alocava: a insercao
    // na free list podia realocar. Aqui o heap e levado ao pior caso — todo slot alocado, e depois os
    // pares devolvidos, um range isolado por slot, a free list no maximo de ranges disjuntos — e os
    // impares devolvidos por cima, cada um fundindo dois vizinhos. Nenhuma alocacao em nenhum passo.
    {
        FTextureSRVHeap H;
        H.Initialize(G.Device.Get());
        std::vector<u32> Slots(FTextureSRVHeap::kCapacity);
        for (u32& Slot : Slots) Slot = H.Allocate(1u);
        CheckEq(H.UsedSlots(), FTextureSRVHeap::kCapacity, "heap: todos os slots alocados");

        u64 AlocacoesPares = 0, AlocacoesImpares = 0;
        {
            FContagemDeAlocacoes Contagem;
            // Os pares em ordem crescente: cada um vira um range isolado no fim da lista.
            for (u32 I = 0; I < FTextureSRVHeap::kCapacity; I += 2u) H.Free(Slots[I], 1u);
            AlocacoesPares = Contagem.Total();
        }
        {
            FContagemDeAlocacoes Contagem;
            // Os impares de tras para frente: cada um funde os dois vizinhos.
            for (u32 I = FTextureSRVHeap::kCapacity - 1u; I < FTextureSRVHeap::kCapacity; I -= 2u)
                H.Free(Slots[I], 1u);
            AlocacoesImpares = Contagem.Total();
        }
        CheckEq64(AlocacoesPares, 0ull,
                  "heap: devolver slots isolados nao aloca, com a free list chegando ao maximo");
        CheckEq64(AlocacoesImpares, 0ull,
                  "heap: devolver slots que fundem vizinhos nao aloca, com a free list no maximo");
        CheckEq(H.UsedSlots(), 0u, "heap: tudo devolvido");
        CheckEq(H.TryAllocate(FTextureSRVHeap::kCapacity), 0u,
                "heap: os ranges voltaram a ser um so, do slot zero ao fim");
    }

    // --- a contagem de alocacoes enxerga a engine ----------------------------------------------
    //
    // Sem isto, "gravar a publicacao nao aloca" passaria tambem com um operator new que nao entrou no
    // link. A primeira preparacao depois do Setup TEM de alocar — linhas, diffs, reservas dos shadows —,
    // e alocacoes da biblioteca da engine precisam aparecer na contagem.
    {
        G.Instances.PrepareFrame(G.Scene, G.Meshes, G.Materials);
        u32 Preparada = GpuScenePublicationOk;
        u64 Alocacoes = 0;
        {
            FContagemDeAlocacoes Contagem;
            Preparada = GpuScenePreparePublication(G.Materials, G.Meshes, G.Instances,
                                                   G.List[G.ProximoSlot].Get());
            Alocacoes = Contagem.Total();
        }
        CheckEq(Preparada, GpuScenePublicationOk, "a primeira preparacao e aceita");
        Check(Alocacoes > 0u, "a contagem enxerga as alocacoes da engine: a primeira preparacao aloca");
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

    // Sem publicacao pendente, as preparacoes aceitam ate sem lista: o frame so e recusado por uma
    // copia que de fato precisaria acontecer.
    Check(G.Materials.PrepareUpload(nullptr),
          "sem publicacao pendente: o material se prepara ate sem lista");
    Check(G.Meshes.PrepareUpload(nullptr),
          "sem publicacao pendente: a malha se prepara ate sem lista");
    Check(G.Instances.PrepareUpload(nullptr),
          "sem publicacao pendente: as instancias se preparam ate sem lista");

    // --- a preparacao vale para UMA lista, e ate a proxima mutacao da tabela --------------------
    //
    // A fase 2 grava o que a fase 1 preparou. Uma mutacao entre as duas deixa a preparacao obsoleta, e
    // grava-la publicaria linhas que nao sao as da tabela. Conferido sobre listas que nunca executam:
    // nenhuma destas gravacoes pode acontecer, e as linhas publicadas nao andam.
    const FGpuMaterialRecord MaterialAntes = G.Materials.PublishedRows()[1];
    {
        wrl::ComPtr<ID3D12CommandAllocator>    AllocL, AllocOutra;
        wrl::ComPtr<ID3D12GraphicsCommandList> L, Outra;
        Hr(G.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&AllocL)),
           "CreateCommandAllocator (preparacao)");
        Hr(G.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&AllocOutra)),
           "CreateCommandAllocator (outra)");
        Hr(G.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, AllocL.Get(), nullptr,
                                       IID_PPV_ARGS(&L)),
           "CreateCommandList (preparacao)");
        Hr(G.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, AllocOutra.Get(), nullptr,
                                       IID_PPV_ARGS(&Outra)),
           "CreateCommandList (outra)");

        Check(!G.Materials.RecordUpload(L.Get(), 0u), "material: sem preparacao, nada e gravado");

        G.Opaco.Constants.BaseColorFactor = { 0.3f, 0.3f, 0.9f, 1.0f };
        G.Materials.MarkDirty();
        Check(G.Materials.PrepareUpload(L.Get()), "material: a preparacao com lista e aceita");
        G.Materials.MarkDirty();
        Check(!G.Materials.RecordUpload(L.Get(), 0u),
              "material: MarkDirty depois de preparar invalida a preparacao");

        Check(G.Materials.PrepareUpload(L.Get()), "material: preparado de novo");
        Check(!G.Materials.RecordUpload(Outra.Get(), 0u),
              "material: a preparacao vale so para a lista em que foi feita");

        const FGpuSceneHandle          H           = G.R(G.IdOpacoB).GpuHandle;
        const FGpuSceneInstanceTransform AtualAntes = G.Instances.PublishedTransforms()[H.Slot];
        auto Mover = [&]() {
            FRenderable& Obj = G.R(G.IdOpacoB);
            Obj.Transform.Position.Y += 1.0f;
            Obj.RefreshWorldBounds();
            G.Scene.BumpTransformsVersion(G.Indice(G.IdOpacoB));
            G.Instances.PrepareFrame(G.Scene, G.Meshes, G.Materials);
        };
        Mover();
        Check(G.Instances.PrepareUpload(L.Get()), "instancias: a preparacao com lista e aceita");
        Mover();
        Check(!G.Instances.RecordUpload(L.Get(), 0u),
              "instancias: um ciclo novo do produtor invalida a preparacao");
        Check(G.Instances.PublishedTransforms()[H.Slot] == AtualAntes,
              "instancias: com a preparacao invalidada, o transform publicado nao anda");

        // As tres sao conferidas antes da primeira copia: com material e malha preparados e as
        // instancias nao, a publicacao do frame nao grava nem o material.
        Check(G.Materials.PrepareUpload(L.Get()) && G.Meshes.PrepareUpload(L.Get()),
              "material e malha preparados");
        Check(!GpuSceneRecordPublication(G.Materials, G.Meshes, G.Instances, L.Get(), 0u),
              "a publicacao do frame recusa uma tabela sem preparacao valida");
        Check(G.Materials.PublishedRows()[1] == MaterialAntes,
              "a publicacao do frame confere as tres preparacoes antes de gravar qualquer uma");

        L->Close();
        Outra->Close();
        G.Drenar("preparacao");
    }
    G.Frame();
    Check(G.Materials.PublishedRows()[1] != MaterialAntes,
          "depois das preparacoes invalidadas, o frame seguinte publica a cor");

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
        Check(!G.Materials.PrepareUpload(nullptr), "classe: a preparacao recusa sem lista");
        Check(!G.Materials.RecordUpload(nullptr, 0u), "classe: preparacao recusada nao grava");
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
        Check(!G.Instances.PrepareUpload(nullptr), "instancias: a preparacao recusa sem lista");
        Check(!G.Instances.RecordUpload(nullptr, 0u), "instancias: preparacao recusada nao grava");
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
        Check(!G.Meshes.PrepareUpload(nullptr), "malha: a preparacao recusa sem lista");
        Check(!G.Meshes.RecordUpload(nullptr, 0u), "malha: preparacao recusada nao grava");
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
    // "Recontar nao aloca" so diz algo se as consultas contadas recontaram: todas as 18 passaram pela
    // contagem — classe, RaytracingOnly, associacao, pool, Setup e Release.
    CheckEq64(G.Remontagens, G.Layout.RebuildAttempts(),
              "toda recontagem do teste passou pela contagem de alocacoes");

    // --- Setup INTERROMPIDO: a tabela sai liberada, e o frame nao existe --------------------------
    //
    // A falha de producao que a fronteira existe para cobrir: um upload buffer recusado (falta de
    // memoria) no meio de um Setup, com o render thread capturando a excecao e pedindo o frame
    // seguinte. O staging e criado depois do buffer, entao cada recusa pega um armazenamento com
    // recurso novo ja criado. Depois de cada falha: armazenamento, mapas, descritores e porta.
    {
        FMaterial* Lista[3]{ &G.Opaco, &G.Mascarado, &G.Troca };
        auto Lanca = [](auto&& _Chamada) {
            try {
                _Chamada();
            } catch (const std::exception&) {
                return true;
            }
            return false;
        };
        auto Montar = [&]() {
            GpuSceneSetupTables(G.Materials, G.Meshes, G.Instances, G.Device.Get(), G.Heap, Lista,
                                G.Scene);
        };

        // Liberadas, as tabelas nao passam por "nada pendente".
        GpuSceneReleaseTables(G.Materials, G.Meshes, G.Instances, G.Heap);
        const u32 SemTabelas = G.Heap.UsedSlots();
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationMaterialsNotBuilt,
                "liberadas: a porta recusa o frame");

        Montar();
        const u32 ComTres = G.Heap.UsedSlots();
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationOk, "montadas: o frame publica");

        // Setup e Release tambem invalidam uma preparacao: depois deles, as linhas preparadas sao de
        // uma tabela que nao existe mais.
        {
            wrl::ComPtr<ID3D12CommandAllocator>    Alloc;
            wrl::ComPtr<ID3D12GraphicsCommandList> L;
            Hr(G.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Alloc)),
               "CreateCommandAllocator (Setup invalida)");
            Hr(G.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&L)),
               "CreateCommandList (Setup invalida)");
            G.Opaco.Constants.BaseColorFactor = { 0.5f, 0.2f, 0.7f, 1.0f };
            G.Materials.MarkDirty();
            Check(G.Materials.PrepareUpload(L.Get()), "Setup: o material se prepara antes");
            G.Materials.SetupForMaterials(G.Device.Get(), G.Heap, Lista);
            Check(!G.Materials.RecordUpload(L.Get(), 0u),
                  "material: Setup depois de preparar invalida a preparacao");
            Check(G.Meshes.PrepareUpload(L.Get()), "Release: a malha se prepara antes");
            G.Meshes.Release(G.Heap);
            Check(!G.Meshes.RecordUpload(L.Get(), 0u),
                  "malha: Release depois de preparar invalida a preparacao");
            G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
            L->Close();
            G.Drenar("Setup invalida");
        }
        CheckEq(G.Heap.UsedSlots(), ComTres, "Setup e Release no meio: a conta de descritores volta");
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationOk, "Setup e Release no meio: o frame publica");

        // Material CRESCENDO: o armazenamento antigo existe e sobrevive ao Create recusado, e e ai
        // que o mapa novo ficaria sobre o buffer antigo, sem nada pendente.
        {
            std::vector<std::unique_ptr<FMaterial>> Extras;
            std::vector<FMaterial*>                 Grande(Lista, Lista + 3);
            for (u32 I = 0; I < 40u; ++I) {
                Extras.push_back(std::make_unique<FMaterial>());
                Grande.push_back(Extras.back().get());
            }
            {
                GpuResources::FUploadFailureInjection Falha(1u);
                Check(Lanca([&] { G.Materials.SetupForMaterials(G.Device.Get(), G.Heap, Grande); }),
                      "material crescendo: o Setup com o staging recusado lanca");
                CheckEq(GpuResources::PendingUploadFailures(), 0u,
                        "material crescendo: a falha foi consumida pelo Setup");
            }
            Check(!G.Materials.IsBuilt(), "material interrompido: a tabela nao esta pronta");
            CheckEq(G.Materials.RowCount(), 0u, "material interrompido: o mapa de linhas foi liberado");
            CheckEq(G.Materials.RowOf(Extras[0].get()), FGpuMaterialTable::kDefaultRow,
                    "material interrompido: o material novo nao resolve linha");
            CheckEq(G.Materials.Srv(), FTextureSRVHeap::kInvalidSlot,
                    "material interrompido: sem descritor");
            CheckEq(G.Heap.UsedSlots(), ComTres - 1u,
                    "material interrompido: o descritor do armazenamento antigo voltou ao heap");
            G.Frame();
            CheckEq(G.Publicacao, GpuScenePublicationMaterialsNotBuilt,
                    "material interrompido: a porta recusa o frame");
        }
        G.Materials.SetupForMaterials(G.Device.Get(), G.Heap, Lista);
        CheckEq(G.Heap.UsedSlots(), ComTres, "material refeito: a conta de descritores volta");
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationOk, "material refeito: o frame publica");

        // Malha nova: o range de descritores de geometria ja foi alocado e escrito quando o staging
        // e recusado.
        {
            G.Meshes.Release(G.Heap);
            const u32 SemMalha = G.Heap.UsedSlots();
            {
                GpuResources::FUploadFailureInjection Falha(1u);
                Check(Lanca([&] { G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene); }),
                      "malha: o Setup com o staging recusado lanca");
                CheckEq(GpuResources::PendingUploadFailures(), 0u,
                        "malha: a falha foi consumida pelo Setup");
            }
            Check(!G.Meshes.IsBuilt(), "malha interrompida: a tabela nao esta pronta");
            CheckEq(G.Meshes.RowCount(), 0u, "malha interrompida: o mapa de linhas foi liberado");
            CheckEq(G.Meshes.RowOf(&G.MalhaA), FGpuMeshTable::kMissingRow,
                    "malha interrompida: a malha nao resolve linha");
            CheckEq(G.Meshes.GeometrySlotOf(&G.MalhaA), 0u,
                    "malha interrompida: nem descritor de geometria");
            Check(G.Meshes.GeometryPools().empty(), "malha interrompida: sem pools");
            CheckEq(G.Meshes.DescriptorCount(), 0u, "malha interrompida: o range foi devolvido");
            CheckEq(G.Heap.UsedSlots(), SemMalha,
                    "malha interrompida: nenhum descritor de geometria preso");
            G.Frame();
            CheckEq(G.Publicacao, GpuScenePublicationMeshesNotBuilt,
                    "malha interrompida: a porta recusa o frame");
        }
        G.Meshes.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        CheckEq(G.Heap.UsedSlots(), ComTres, "malha refeita: a conta de descritores volta");
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationOk, "malha refeita: o frame publica");

        // Instancias interrompidas na TERCEIRA coluna: meta e bounds ja existem quando o staging do
        // transform e recusado. E o caso que conferir so a meta aprovava.
        {
            G.Instances.Release(G.Heap);
            const u32 SemInstancias = G.Heap.UsedSlots();
            {
                GpuResources::FUploadFailureInjection Falha(1u, 2u);
                Check(Lanca([&] { G.Instances.SetupForScene(G.Device.Get(), G.Heap, G.Scene); }),
                      "instancias: o Setup com o terceiro staging recusado lanca");
                CheckEq(GpuResources::PendingUploadFailures(), 0u,
                        "instancias: a falha foi consumida pelo terceiro staging");
            }
            Check(!G.Instances.IsBuilt(), "instancias interrompidas: a tabela nao esta pronta");
            CheckEq(G.Instances.Capacity(), 0u, "instancias interrompidas: sem capacidade");
            Check(G.Instances.MetaResource() == nullptr && G.Instances.BoundsResource() == nullptr,
                  "instancias interrompidas: meta e bounds, ja criadas, foram soltas");
            CheckEq(G.Instances.MetaSrv(), FTextureSRVHeap::kInvalidSlot,
                    "instancias interrompidas: sem descritor de meta");
            CheckEq(G.Heap.UsedSlots(), SemInstancias,
                    "instancias interrompidas: nenhum descritor preso");
            G.Frame();
            CheckEq(G.Publicacao, GpuScenePublicationInstancesNotBuilt,
                    "instancias interrompidas: a porta recusa o frame");
        }
        G.Instances.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        CheckEq(G.Heap.UsedSlots(), ComTres, "instancias refeitas: a conta de descritores volta");
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationOk, "instancias refeitas: o frame publica");

        // Falha ENTRE os setups: o material completa — com a mesma lista ele nem recria recurso —, e
        // a malha, sem armazenamento, falha. A transacao libera as tres, inclusive a que completou.
        {
            G.Meshes.Release(G.Heap);
            {
                GpuResources::FUploadFailureInjection Falha(1u);
                Check(Lanca(Montar), "transacao: a malha recusada lanca");
                CheckEq(GpuResources::PendingUploadFailures(), 0u,
                        "transacao: a falha foi consumida pela malha");
            }
            Check(!G.Materials.IsBuilt(), "transacao: o material que completou tambem sai liberado");
            Check(!G.Meshes.IsBuilt(), "transacao: a malha que falhou sai liberada");
            Check(!G.Instances.IsBuilt(), "transacao: as instancias, que nao rodaram, saem liberadas");
            CheckEq(G.Heap.UsedSlots(), SemTabelas, "transacao: nenhum descritor preso");
            G.Frame();
            CheckEq(G.Publicacao, GpuScenePublicationMaterialsNotBuilt,
                    "transacao: a porta recusa o frame");
        }
        Montar();
        CheckEq(G.Heap.UsedSlots(), ComTres, "transacao refeita: a conta de descritores volta");
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationOk, "transacao refeita: o frame publica as tres");
        Check(!G.Materials.PublishedRows().empty() && !G.Meshes.PublishedRows().empty() &&
                  !G.Instances.PublishedMeta().empty(),
              "transacao refeita: as tres tem linhas publicadas");

        // Falta de memoria DENTRO do rollback. Com as tres montadas e toda alocacao da thread
        // falhando, o Setup do material lanca na primeira, e o rollback — o dele e o da transacao —
        // tem de liberar as tres sem pedir memoria nenhuma: sem descritor preso e sem buffer vivo.
        {
            bool BadAlloc = false;
            {
                FFaltaDeMemoria Falta;
                try {
                    Montar();
                } catch (const std::bad_alloc&) {
                    BadAlloc = true;
                }
            }
            Check(BadAlloc, "sem memoria: a transacao lanca bad_alloc");
            Check(!G.Materials.IsBuilt() && !G.Meshes.IsBuilt() && !G.Instances.IsBuilt(),
                  "sem memoria: o rollback libera as tres tabelas");
            Check(G.Materials.Resource() == nullptr && G.Meshes.Resource() == nullptr &&
                      G.Instances.MetaResource() == nullptr &&
                      G.Instances.BoundsResource() == nullptr &&
                      G.Instances.TransformResource() == nullptr &&
                      G.Instances.PrevTransformResource() == nullptr,
                  "sem memoria: nenhum buffer ficou vivo");
            CheckEq(G.Heap.UsedSlots(), SemTabelas, "sem memoria: nenhum descritor ficou preso");
            G.Frame();
            CheckEq(G.Publicacao, GpuScenePublicationMaterialsNotBuilt,
                    "sem memoria: a porta recusa o frame");
        }
        Montar();
        CheckEq(G.Heap.UsedSlots(), ComTres, "depois da falta de memoria: a conta de descritores volta");
        G.Frame();
        CheckEq(G.Publicacao, GpuScenePublicationOk, "depois da falta de memoria: o frame publica");

        // A cena CRESCE alem da tabela sem o Setup que acompanha: o produtor nao cabe, e o frame nao
        // existe ate o Setup chegar.
        {
            u32 Acrescentados = 0;
            while (G.Scene.GpuSlots().Capacity() <= G.Instances.Capacity() &&
                   Acrescentados < 100000u) {
                FRenderable Novo;
                Novo.Mesh     = &G.MalhaA;
                Novo.Material = &G.Opaco;
                G.Scene.AddRenderable(Novo);
                ++Acrescentados;
            }
            Check(G.Scene.GpuSlots().Capacity() > G.Instances.Capacity(),
                  "cena maior: a cena passou da capacidade da tabela");
            G.Frame();
            Check(!G.Instances.FitsScene(), "cena maior: o produtor ve que ela nao cabe");
            CheckEq(G.Publicacao, GpuScenePublicationSceneExceedsTable,
                    "cena maior: a porta recusa o frame");

            G.Instances.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
            G.Frame();
            Check(G.Instances.FitsScene(), "cena maior, com Setup: a cena cabe");
            CheckEq(G.Publicacao, GpuScenePublicationOk, "cena maior, com Setup: o frame publica");
        }
    }

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
} catch (const std::exception& E) {
    std::cerr << "Smile.GpuIndirectLayout: excecao -- " << E.what() << '\n';
    return 2;
}
