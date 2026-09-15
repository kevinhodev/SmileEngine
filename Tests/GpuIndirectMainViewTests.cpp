// A cadeia do main view INDIRETO gravada pela engine (Fase 5 do GPU-DRIVEN-PLAN), sobre tabelas, cena e
// fila de verdade, executada em WARP.
//
// Cada frame segue a ordem do RendererFrame: produtor e preparacao da publicacao das tabelas, `Prepare` da
// cadeia ANTES de abrir a lista; depois, gravacao da publicacao e `Record` da cadeia — contado, porque nao
// pode alocar. No fim da lista, o teste le de volta TUDO o que a cadeia escreveu: saidas da expansao,
// contagens e cursores por lote, argumentos, count buffer, os tres conjuntos de contadores e o status. A
// imagem esperada sai das referencias CPU do GpuClassify.h, calculadas no momento da gravacao sobre as
// linhas PUBLICADAS, a lista de candidatos e o layout que a propria cadeia usou.
//
// O que so este teste enxerga, em ordem de gravidade:
//
//  1. o RESET unico incluir os contadores do expand, e a finalizacao rodar SEMPRE: um frame vazio depois
//     de um cheio publica contagens e contadores zerados, e nao os do cheio;
//  2. staging, cbuffers e tabelas de descritores POR FRAME SLOT: dois frames gravados antes de qualquer um
//     executar, com layouts diferentes, conferem cada um a sua imagem;
//  3. a tabela de regioes subir quando a versao do layout muda, e so entao;
//  4. uma cadeia recusada nao gravar NADA: a leitura do fim da lista ve a saida da cadeia anterior, e o
//     upload recusado continua pendente para a seguinte;
//  5. `TotalCommands <= capacidade alocada` antes do dispatch, e as outras portas de capacidade, cada uma
//     pelo seu motivo;
//  6. a cadeia terminar cada recurso no estado de REPOUSO. Quem prova e a leitura do fim da lista: ela
//     parte do repouso de cada um, e a camada de debug reprova a barreira quando a cadeia nao terminou
//     nele. Sozinha, entre duas listas, a camada de debug NAO compara o estado de buffer — medido: sem a
//     barreira de volta dos argumentos e sem a leitura deles, nenhum erro aparece;
//  7. a telemetria da GPU ser a da cadeia anterior do mesmo slot;
//  8. um CANARIO produzido pela propria cena — um candidato que o expand recusa — invalidar o frame
//     inteiro, com a marca do expand, enquanto o masked da cena, que e politica, deixa o frame fechar.
//
// O que ele NAO enxerga: a tabela de descritores ser POR frame slot. Os dois frames em voo copiam os
// mesmos descritores, dos mesmos recursos, e reescrever um descritor com o mesmo conteudo nao muda o que a
// GPU le — medido: com a tabela do slot 0 para os dois, fica verde. O defeito so teria efeito com a origem
// trocada entre dois frames em voo, e trocar a origem exige Setup de tabela, que drena as filas.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <malloc.h>
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

#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Scene/GpuIndirectMainView.h"
#include "Smile/Scene/Scene.h"

using namespace Smile;
namespace wrl = Microsoft::WRL;

// === Contagem de alocacoes =========================================================================
//
// O `Record` roda com a lista do frame aberta. "Nao aloca" so em comentario nao se sustenta: o executavel
// troca o operator new global e conta as alocacoes da thread durante a gravacao. O D3D12 aloca pelos
// proprios heaps, fora deste operator: o que se conta e o codigo da engine.
namespace {
    thread_local bool GContarAlocacoes = false;
    thread_local u64  GAlocacoes       = 0;

    void* Alocar(std::size_t _Bytes) {
        if (GContarAlocacoes) ++GAlocacoes;
        if (void* P = std::malloc(_Bytes != 0 ? _Bytes : 1)) return P;
        throw std::bad_alloc();
    }

    void* AlocarAlinhado(std::size_t _Bytes, std::align_val_t _Alinhamento) {
        if (GContarAlocacoes) ++GAlocacoes;
        if (void* P = _aligned_malloc(_Bytes != 0 ? _Bytes : 1,
                                      static_cast<std::size_t>(_Alinhamento)))
            return P;
        throw std::bad_alloc();
    }

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

// E a gravacao nao LANCA, pela assinatura.
static_assert(noexcept(std::declval<FGpuIndirectMainView&>().Record(
                  nullptr, 0u, std::declval<FTextureSRVHeap&>(),
                  std::declval<const FGpuMaterialTable&>(), std::declval<const FGpuMeshTable&>(),
                  std::declval<const FGpuSceneInstanceTable&>(),
                  std::span<const FGpuIndirectPassSignature>{})),
              "gravar a cadeia e noexcept");

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

    bool MesmoComando(const FGpuIndirectDrawArgs& A, const FGpuIndirectDrawArgs& B) {
        return std::memcmp(&A, &B, sizeof(A)) == 0;
    }

    std::vector<FGpuIndirectDrawArgs> Ordenados(const FGpuIndirectDrawArgs* _Inicio, u32 _N) {
        std::vector<FGpuIndirectDrawArgs> V(_Inicio, _Inicio + _N);
        std::sort(V.begin(), V.end(), [](const FGpuIndirectDrawArgs& A, const FGpuIndirectDrawArgs& B) {
            return std::memcmp(&A, &B, sizeof(A)) < 0;
        });
        return V;
    }

    // A expansao como a GPU a faz: POSICIONAL, com a sentinela onde o candidato nao passou, e os quatro
    // contadores. A referencia `ExpandCandidateRanges` so devolve slots, sem validar generation.
    std::vector<u32> ExpandirComoAGpu(std::span<const FInstanceRange> _Ranges,
                                      std::span<const FGpuCandidateId> _Ids,
                                      std::span<const FGpuSceneInstanceMeta> _Meta,
                                      std::array<u32, GpuExpandCounterCount>& _Contadores) {
        std::vector<u32> Saida;
        _Contadores = {};
        for (const FInstanceRange& R : _Ranges) {
            for (u32 I = 0; I < R.CandidateCount; ++I) {
                const FGpuCandidateId& C = _Ids[R.FirstCandidate + I];
                if (C.Slot >= _Meta.size() || C.Generation == 0u ||
                    _Meta[C.Slot].Generation != C.Generation) {
                    ++_Contadores[GpuExpandCounterInvalidSlot];
                    Saida.push_back(kGpuExpandNoSlot);
                } else if ((_Meta[C.Slot].Flags & GpuSceneInstanceRaytracingOnly) != 0u) {
                    ++_Contadores[GpuExpandCounterRtOnly];
                    Saida.push_back(kGpuExpandNoSlot);
                } else {
                    ++_Contadores[GpuExpandCounterExpanded];
                    Saida.push_back(C.Slot);
                }
            }
        }
        return Saida;
    }

    // A imagem que a GPU tem de ter depois de uma cadeia: calculada na GRAVACAO, pelas referencias CPU,
    // sobre as mesmas entradas que a cadeia recebeu.
    struct FEsperado {
        bool Valido = false;
        u32  Status = 0;
        u32  Reason = 0;
        u32  Lotes  = 0;
        std::vector<u32>                        Slots;
        std::vector<u32>                        Contagens, Cursores, Draw;
        std::array<u32, GpuExpandCounterCount>  Expand{};
        std::vector<u32>                        Classify, Scatter;
        std::vector<FGpuIndirectRegion>         Regioes;
        std::vector<FGpuIndirectDrawArgs>       Args;
    };

    // Um recurso da cadeia copiado para leitura no fim da lista.
    struct FLeitura {
        wrl::ComPtr<ID3D12Resource> Buffer;
        std::vector<u8>             Bytes;
    };

    struct FLeituras {
        FLeitura Draw, Contagens, Cursores, Slots, Args, Expand, Classify, Scatter, Status;
        u64      Ultimo = 0; // o uso da lista em que foram gravadas
    };

    struct FResultado {
        u32 Publicacao = GpuScenePublicationOk;
        u32 Preparo    = GpuMainViewOk;
        u32 Gravacao   = GpuMainViewNotBuilt;
    };

    struct FRig {
        wrl::ComPtr<ID3D12Device>              Device;
        wrl::ComPtr<ID3D12InfoQueue>           Info;
        wrl::ComPtr<ID3D12CommandQueue>        Queue;
        wrl::ComPtr<ID3D12Fence>               Fence;
        u64                                    FenceValue = 0;
        wrl::ComPtr<ID3D12CommandAllocator>    Allocator[FGpuIndirectMainView::kFrameSlots];
        wrl::ComPtr<ID3D12GraphicsCommandList> List[FGpuIndirectMainView::kFrameSlots];
        u32                                    ProximoSlot = 0;

        FTextureSRVHeap Heap;
        FScene          Scene;

        wrl::ComPtr<ID3D12Resource> PoolA, PoolB;
        FGpuMesh  MalhaA, MalhaB;
        FMaterial Opaco, Mascarado, Troca;

        FGpuMeshTable          Meshes;
        FGpuMaterialTable      Materials;
        FGpuSceneInstanceTable Instances;
        FGpuIndirectMainView   View;

        // Um passe de desenho: a command signature de PRODUCAO contra uma root signature com o bloco de
        // constantes do main view. E o que a porta da gravacao dos draws confere antes da cadeia.
        wrl::ComPtr<ID3D12RootSignature>    DrawRoot;
        wrl::ComPtr<ID3D12CommandSignature> Signature;
        std::array<FGpuIndirectPassSignature, 1> Passe{};
        std::array<FGpuIndirectPassSignature, 1> PasseSemSignature{};

        FEsperado Esperado[FGpuIndirectMainView::kFrameSlots];
        FLeituras Leituras[FGpuIndirectMainView::kFrameSlots];
        // A imagem da ultima cadeia GRAVADA, na ordem de gravacao: e o que a GPU tem depois de uma cadeia
        // recusada. Invalidada por um Setup da cadeia, que troca os recursos.
        FEsperado UltimaCadeia;
        // Houve cadeia gravada no uso anterior de cada slot: e a telemetria que a proxima gravacao coleta.
        bool      CadeiaNoSlot[FGpuIndirectMainView::kFrameSlots]{};
        FEsperado TelemetriaEsperada[FGpuIndirectMainView::kFrameSlots];
        u32       TelemetriasConferidas = 0;
        u32       GravacoesContadas     = 0;

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
            for (u32 S = 0; S < FGpuIndirectMainView::kFrameSlots; ++S) {
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

            for (u64& Id : IdOpaco) Id = Adicionar(&MalhaA, &Opaco);
            for (u64& Id : IdTroca) Id = Adicionar(&MalhaB, &Troca);
            IdMascarado = Adicionar(&MalhaA, &Mascarado);
            IdOpacoB    = Adicionar(&MalhaB, &Opaco);

            Meshes.SetupForScene(Device.Get(), Heap, Scene);
            FMaterial* Lista[3]{ &Opaco, &Mascarado, &Troca };
            Materials.SetupForMaterials(Device.Get(), Heap, Lista);
            Instances.SetupForScene(Device.Get(), Heap, Scene);
            if (!Instances.IsBuilt() || Meshes.GeometryPools().size() != 2u)
                throw std::runtime_error("as tabelas do aparelho nao foram construidas");

            D3D12_ROOT_PARAMETER P{};
            P.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            P.Constants.ShaderRegister = 4;
            P.Constants.Num32BitValues = GpuDrawConstants::kCount;
            P.ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC RD{};
            RD.NumParameters = 1;
            RD.pParameters   = &P;
            RD.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            wrl::ComPtr<ID3DBlob> Serial, Erro;
            Hr(D3D12SerializeRootSignature(&RD, D3D_ROOT_SIGNATURE_VERSION_1, &Serial, &Erro),
               "SerializeRootSignature (passe)");
            Hr(Device->CreateRootSignature(0, Serial->GetBufferPointer(), Serial->GetBufferSize(),
                                           IID_PPV_ARGS(&DrawRoot)),
               "CreateRootSignature (passe)");
            D3D12_INDIRECT_ARGUMENT_DESC Descs[3]{};
            const D3D12_COMMAND_SIGNATURE_DESC SigDesc = MakeIndirectDrawSignatureDesc(Descs, 0u);
            Hr(Device->CreateCommandSignature(&SigDesc, DrawRoot.Get(), IID_PPV_ARGS(&Signature)),
               "CreateCommandSignature (passe)");
            Passe[0]             = { Signature.Get(), GpuPipelineClassOpaqueOneSided };
            PasseSemSignature[0] = { nullptr, GpuPipelineClassOpaqueOneSided };

            View.Initialize(Device.Get());
            Drenar("construcao");
        }

        u64 Adicionar(FGpuMesh* _Malha, FMaterial* _Material) {
            FRenderable R;
            R.Mesh         = _Malha;
            R.Material     = _Material;
            R.LocalAABBMin = { -1.0f, -1.0f, -1.0f };
            R.LocalAABBMax = { 1.0f, 1.0f, 1.0f };
            R.RefreshWorldBounds();
            return Scene.AddRenderable(R).Id;
        }

        FRenderable& R(u64 _Id) { return *Scene.FindRenderable(_Id); }
        u32 Indice(u64 _Id) const { return static_cast<u32>(Scene.IndexOfRenderable(_Id)); }

        void Mostrar(u64 _Id, bool _Visivel) {
            R(_Id).Visible = _Visivel;
            Scene.BumpStaticCastersVersion(Indice(_Id));
        }

        // Um Setup da cadeia troca os recursos: o que a GPU tinha antes nao vale mais como imagem.
        void Montar(u32 _Slots, u32 _Pools) {
            View.Setup(Device.Get(), Heap, _Slots, _Pools);
            UltimaCadeia = {};
            for (bool& B : CadeiaNoSlot) B = false;
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

        // A imagem que a cadeia recem-gravada tem de produzir.
        FEsperado Oraculo() {
            FEsperado E;
            E.Valido = true;
            const FGpuSceneCandidates& C    = View.Candidates();
            const auto                 Meta = Instances.PublishedMeta();
            E.Slots = ExpandirComoAGpu(C.Ranges(), C.InstanceIds(), Meta, E.Expand);

            const FGpuClassifyInput In{ E.Slots, Meta, Materials.PublishedRows(), Meshes.PublishedRows(),
                                        static_cast<u32>(Meshes.GeometryPools().size()),
                                        View.BatchCapacity() };
            ClassifyCandidates(In, E.Contagens, E.Classify);

            const FGpuIndirectLayout& L = View.Layout();
            E.Regioes.assign(L.Regions().begin(), L.Regions().end());
            E.Lotes = static_cast<u32>(E.Regioes.size());
            E.Cursores.assign(View.BatchCapacity(), 0u);
            E.Args.assign(View.SlotCapacity(), FGpuIndirectDrawArgs{});
            ScatterIndirectArgs(In, L.Regions(), L.Version(), E.Contagens, E.Cursores, E.Args, E.Scatter);

            E.Contagens.resize(E.Lotes);
            E.Cursores.resize(E.Lotes);
            E.Status = FinalizeDrawCounts(E.Contagens, E.Regioes, E.Scatter, E.Cursores, E.Classify,
                                          E.Expand, static_cast<u32>(E.Slots.size()), E.Draw, E.Reason);
            return E;
        }

        // Copia um recurso da cadeia para leitura, a partir do repouso dele e de volta.
        void Ler(ID3D12GraphicsCommandList* _L, FLeitura& _Lt, ID3D12Resource* _R,
                 D3D12_RESOURCE_STATES _Repouso) {
            _Lt.Bytes.clear();
            _Lt.Buffer.Reset();
            if (!_R) return;
            const u64 Bytes = _R->GetDesc().Width;
            D3D12_HEAP_PROPERTIES HP{};
            HP.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC RD{};
            RD.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            RD.Width            = Bytes;
            RD.Height           = 1;
            RD.DepthOrArraySize = 1;
            RD.MipLevels        = 1;
            RD.SampleDesc.Count = 1;
            RD.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&_Lt.Buffer)),
               "CreateCommittedResource (leitura)");
            const bool Transita = _Repouso != D3D12_RESOURCE_STATE_COPY_SOURCE;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = _R;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = _Repouso;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            if (Transita) _L->ResourceBarrier(1, &B);
            _L->CopyResource(_Lt.Buffer.Get(), _R);
            std::swap(B.Transition.StateBefore, B.Transition.StateAfter);
            if (Transita) _L->ResourceBarrier(1, &B);
        }

        // Grava um frame no slot, na ordem do RendererFrame.
        FResultado Gravar(u32 _Slot, std::span<const FGpuIndirectPassSignature> _Passes) {
            FResultado Res;
            ID3D12GraphicsCommandList* L = List[_Slot].Get();
            Instances.PrepareFrame(Scene, Meshes, Materials);
            Res.Publicacao = GpuScenePreparePublication(Materials, Meshes, Instances, L);
            if (Res.Publicacao != GpuScenePublicationOk) return Res;
            Res.Preparo = View.Prepare(Scene, Meshes, L);

            Hr(Allocator[_Slot]->Reset(), "Reset do allocator");
            Hr(L->Reset(Allocator[_Slot].Get(), nullptr), "Reset da lista");
            Check(GpuSceneRecordPublication(Materials, Meshes, Instances, L, _Slot),
                  "a publicacao preparada e gravada");

            u64 Alocacoes = 0;
            {
                FContagemDeAlocacoes Contagem;
                Res.Gravacao = View.Record(L, _Slot, Heap, Materials, Meshes, Instances, _Passes);
                Alocacoes    = Contagem.Total();
            }
            CheckEq64(Alocacoes, 0ull, "gravar a cadeia nao aloca");
            ++GravacoesContadas;

            // A telemetria coletada no inicio da gravacao e a da cadeia ANTERIOR deste slot.
            if (CadeiaNoSlot[_Slot]) {
                const FGpuMainViewGpuCounters& T = View.LastGpuCounters();
                const FEsperado&               E = TelemetriaEsperada[_Slot];
                Check(T.Valid, "telemetria: a cadeia anterior do slot foi coletada");
                CheckEq(T.Status, E.Status, "telemetria: status da cadeia anterior do slot");
                CheckEq(T.Reason, E.Reason, "telemetria: motivo da cadeia anterior do slot");
                Check(std::equal(T.Expand.begin(), T.Expand.end(), E.Expand.begin()),
                      "telemetria: contadores do expand da cadeia anterior do slot");
                Check(std::equal(T.Classify.begin(), T.Classify.end(), E.Classify.begin()),
                      "telemetria: contadores do classify da cadeia anterior do slot");
                Check(std::equal(T.Scatter.begin(), T.Scatter.end(), E.Scatter.begin()),
                      "telemetria: contadores do scatter da cadeia anterior do slot");
                ++TelemetriasConferidas;
            }

            if (Res.Gravacao == GpuMainViewOk) {
                UltimaCadeia              = Oraculo();
                CadeiaNoSlot[_Slot]       = true;
                TelemetriaEsperada[_Slot] = UltimaCadeia;
            } else {
                CadeiaNoSlot[_Slot] = false;
            }
            // Recusada, a GPU continua com a imagem da ultima cadeia gravada.
            Esperado[_Slot] = UltimaCadeia;

            FLeituras& Lt = Leituras[_Slot];
            Ler(L, Lt.Draw, View.DrawCounts(), FGpuIndirectMainView::kDrawCountsRestState);
            Ler(L, Lt.Contagens, View.BatchCounts(), FGpuIndirectMainView::kBatchRestState);
            Ler(L, Lt.Cursores, View.BatchCursors(), FGpuIndirectMainView::kBatchRestState);
            Ler(L, Lt.Slots, View.ExpandedSlots(), FGpuIndirectMainView::kExpandedRestState);
            Ler(L, Lt.Args, View.IndirectArgs(), FGpuIndirectMainView::kArgsRestState);
            Ler(L, Lt.Expand, View.ExpandCounters(), FGpuIndirectMainView::kCountersRestState);
            Ler(L, Lt.Classify, View.ClassifyCounters(), FGpuIndirectMainView::kCountersRestState);
            Ler(L, Lt.Scatter, View.ScatterCounters(), FGpuIndirectMainView::kCountersRestState);
            Ler(L, Lt.Status, View.StatusBuffer(), FGpuIndirectMainView::kCountersRestState);

            const HRESULT Fechou = L->Close();
            if (FAILED(Fechou)) Drenar("gravacao");
            Hr(Fechou, "Close");
            return Res;
        }

        // Executa a lista do slot, espera a fence e confere a imagem.
        void Executar(u32 _Slot, std::string_view _Caso) {
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

            FLeituras& Lt = Leituras[_Slot];
            for (FLeitura* P : { &Lt.Draw, &Lt.Contagens, &Lt.Cursores, &Lt.Slots, &Lt.Args, &Lt.Expand,
                                 &Lt.Classify, &Lt.Scatter, &Lt.Status }) {
                if (!P->Buffer) continue;
                const u64 Bytes = P->Buffer->GetDesc().Width;
                P->Bytes.resize(static_cast<size_t>(Bytes));
                u8* M = nullptr;
                Hr(P->Buffer->Map(0, nullptr, reinterpret_cast<void**>(&M)), "Map (leitura)");
                std::memcpy(P->Bytes.data(), M, static_cast<size_t>(Bytes));
                P->Buffer->Unmap(0, nullptr);
            }
            Conferir(_Slot, _Caso);
        }

        void Conferir(u32 _Slot, std::string_view _Caso) {
            const FEsperado& E  = Esperado[_Slot];
            FLeituras&       Lt = Leituras[_Slot];
            const std::string C(_Caso);
            if (!E.Valido) return; // recursos novos, sem cadeia gravada ainda: nada a comparar

            auto U32 = [](const FLeitura& _L, size_t _I) {
                u32 V = 0;
                if ((_I + 1) * 4 <= _L.Bytes.size()) std::memcpy(&V, _L.Bytes.data() + _I * 4, 4);
                return V;
            };
            CheckEq(U32(Lt.Status, 0), E.Status, C + ": status");
            CheckEq(U32(Lt.Status, 1), E.Reason, C + ": motivo da finalizacao");
            for (u32 K = 0; K < GpuExpandCounterCount; ++K)
                CheckEq(U32(Lt.Expand, K), E.Expand[K],
                        C + ": contador do expand " + std::to_string(K));
            for (u32 K = 0; K < GpuClassifyOutcomeCount; ++K)
                CheckEq(U32(Lt.Classify, K), E.Classify[K],
                        C + ": contador do classify " + std::to_string(K));
            for (u32 K = 0; K < GpuScatterCounterCount; ++K)
                CheckEq(U32(Lt.Scatter, K), E.Scatter[K],
                        C + ": contador do scatter " + std::to_string(K));
            for (size_t I = 0; I < E.Slots.size(); ++I)
                CheckEq(U32(Lt.Slots, I), E.Slots[I], C + ": saida da expansao " + std::to_string(I));
            for (u32 B = 0; B < E.Lotes; ++B) {
                CheckEq(U32(Lt.Contagens, B), E.Contagens[B], C + ": contagem do lote " + std::to_string(B));
                CheckEq(U32(Lt.Cursores, B), E.Cursores[B], C + ": cursor do lote " + std::to_string(B));
                CheckEq(U32(Lt.Draw, B), E.Draw[B], C + ": count buffer do lote " + std::to_string(B));

                // Os comandos da regiao que o count buffer publica, como MULTICONJUNTO: a ordem dentro do
                // lote e a de chegada ao cursor, que nao e deterministica.
                const FGpuIndirectRegion& R = E.Regioes[B];
                const u32 N = E.Draw[B];
                const u64 Fim = (static_cast<u64>(R.Offset) + N) * sizeof(FGpuIndirectDrawArgs);
                if (N == 0u) continue;
                if (Fim > Lt.Args.Bytes.size() || R.Offset + N > E.Args.size()) {
                    Check(false, C + ": a regiao do lote cabe nos argumentos lidos");
                    continue;
                }
                std::vector<FGpuIndirectDrawArgs> Gpu(N);
                std::memcpy(Gpu.data(), Lt.Args.Bytes.data() + R.Offset * sizeof(FGpuIndirectDrawArgs),
                            N * sizeof(FGpuIndirectDrawArgs));
                const auto GpuOrd = Ordenados(Gpu.data(), N);
                const auto CpuOrd = Ordenados(E.Args.data() + R.Offset, N);
                bool Iguais = true;
                for (u32 I = 0; I < N; ++I) Iguais = Iguais && MesmoComando(GpuOrd[I], CpuOrd[I]);
                Check(Iguais, C + ": os comandos da regiao do lote " + std::to_string(B));
            }
        }

        // Um frame, gravado e executado antes do seguinte. Sem publicacao, nao ha frame e o slot nao anda.
        FResultado Frame(std::span<const FGpuIndirectPassSignature> _Passes, std::string_view _Caso) {
            const u32        Slot = ProximoSlot;
            const FResultado Res  = Gravar(Slot, _Passes);
            if (Res.Publicacao != GpuScenePublicationOk) return Res;
            ProximoSlot = (ProximoSlot + 1u) % FGpuIndirectMainView::kFrameSlots;
            Executar(Slot, _Caso);
            return Res;
        }
    };
}

int main() try {
    FRig G;
    G.Construir();
    const std::span<const FGpuIndirectPassSignature> Passes(G.Passe);

    // --- sem Setup nao ha cadeia ------------------------------------------------------------------
    {
        FGpuIndirectMainView Vazia;
        CheckEq(Vazia.Prepare(G.Scene, G.Meshes, G.List[0].Get()), GpuMainViewNotBuilt,
                "sem Setup, a preparacao recusa");
    }

    const u32 DescritoresSemCadeia = G.Heap.UsedSlots();
    G.Montar(G.Instances.Capacity(), 2u);
    Check(G.View.IsBuilt(), "o Setup monta a cadeia");
    CheckEq(G.Heap.UsedSlots(), DescritoresSemCadeia + G.View.DescriptorCount(),
            "o Setup aloca os descritores que declara");
    {
        // O custo por slot de instancia: candidato (8 B), saida da expansao (4 B) e comando (28 B).
        const u64 Slots = G.View.SlotCapacity(), Lotes = 8u;
        CheckEq(G.View.BatchCapacity(), 8u, "dois pools: oito lotes");
        const u64 Esperado = Slots * (8u + 4u + 28u) + FGpuIndirectMainView::kRangeCapacity * (8u + 4u) +
                             Lotes * (12u + 3u * 4u) +
                             (static_cast<u32>(GpuExpandCounterCount) + GpuClassifyOutcomeCount +
                              GpuScatterCounterCount + 2u) * 4u;
        CheckEq64(G.View.ResidentBytes(), Esperado, "os bytes residentes da cadeia");
    }
    CheckEq(G.View.Prepare(G.Scene, G.Meshes, nullptr), GpuMainViewNoCommandList,
            "sem command list, a preparacao recusa");

    // --- frame cheio, e o seguinte sem mudanca -----------------------------------------------------
    {
        FResultado R = G.Frame(Passes, "cheio");
        CheckEq(R.Gravacao, GpuMainViewOk, "cheio: a cadeia e gravada");
        CheckEq(G.View.Stats().Outputs, 7u, "cheio: sete candidatos");
        CheckEq(G.View.Stats().CandidateRowsUploaded, 7u, "cheio: a lista inteira sobe");
        CheckEq64(G.View.Stats().RegionUploads, 1ull, "cheio: a tabela de regioes sobe");
        CheckEq(G.View.Layout().TotalCommands(), 6u, "cheio: seis comandos possiveis (o mascarado fica fora)");
        // O oraculo escrito a mao, contra o calculado: os tres opacos no pool A e as duas trocas mais o
        // opaco no pool B; o mascarado sai por politica.
        CheckEq(G.UltimaCadeia.Status, 1u, "cheio: o frame fecha");
        CheckEq(G.UltimaCadeia.Draw.size() > 1 ? G.UltimaCadeia.Draw[0] : 0u, 3u, "cheio: pool A desenha 3");
        CheckEq(G.UltimaCadeia.Draw.size() > 1 ? G.UltimaCadeia.Draw[1] : 0u, 3u, "cheio: pool B desenha 3");
        CheckEq(G.UltimaCadeia.Classify[GpuClassifyNotEligible], 1u, "cheio: o mascarado e politica");

        R = G.Frame(Passes, "sem mudanca");
        CheckEq(R.Gravacao, GpuMainViewOk, "sem mudanca: a cadeia e gravada");
        CheckEq(G.View.Stats().CandidateRowsUploaded, 0u, "sem mudanca: nenhuma linha de candidato sobe");
        CheckEq64(G.View.Stats().RegionUploads, 1ull, "sem mudanca: as regioes nao sobem de novo");

        R = G.Frame(Passes, "terceiro frame");
        CheckEq(R.Gravacao, GpuMainViewOk, "terceiro frame: a cadeia e gravada");
        CheckEq(G.TelemetriasConferidas, 1u, "o terceiro frame confere a telemetria do primeiro");
    }

    // --- troca de classe: layout novo, regioes sobem ------------------------------------------------
    {
        const u32 VersaoAntes = G.View.Layout().Version();
        G.Troca.TwoSided = true;
        G.Materials.MarkDirty();
        const FResultado R = G.Frame(Passes, "classe");
        CheckEq(R.Gravacao, GpuMainViewOk, "classe: a cadeia e gravada");
        Check(G.View.Layout().Version() > VersaoAntes, "classe: o layout e remontado numa versao nova");
        CheckEq64(G.View.Stats().RegionUploads, 2ull, "classe: a tabela de regioes sobe de novo");
        CheckEq(G.UltimaCadeia.Draw[1], 1u, "classe: o pool B fica so com o opaco");
    }

    // --- dois frames em voo, com layouts diferentes -------------------------------------------------
    //
    // Os dois sao gravados antes de qualquer um executar. O primeiro sobe a tabela de regioes de uma
    // versao, o segundo a de outra, e cada um le o cbuffer, o staging e as views do SEU slot.
    {
        const u32 A = G.ProximoSlot, B = (A + 1u) % FGpuIndirectMainView::kFrameSlots;
        G.Troca.TwoSided = false;
        G.Materials.MarkDirty();
        const FResultado RA = G.Gravar(A, Passes);
        CheckEq(RA.Gravacao, GpuMainViewOk, "em voo: o primeiro frame grava a cadeia");
        const u32 VersaoA = G.View.Layout().Version();

        G.Troca.TwoSided = true;
        G.Materials.MarkDirty();
        const FResultado RB = G.Gravar(B, Passes);
        CheckEq(RB.Gravacao, GpuMainViewOk, "em voo: o segundo frame grava a cadeia");
        Check(G.View.Layout().Version() != VersaoA, "em voo: os dois frames tem layouts diferentes");

        G.Executar(A, "em voo, primeiro");
        G.Executar(B, "em voo, segundo");
        CheckEq(G.Esperado[A].Draw[1], 3u, "em voo: o primeiro desenha as trocas no pool B");
        CheckEq(G.Esperado[B].Draw[1], 1u, "em voo: o segundo nao");
        G.ProximoSlot = A;
        G.Troca.TwoSided = false;
        G.Materials.MarkDirty();
        CheckEq(G.Frame(Passes, "depois de em voo").Gravacao, GpuMainViewOk, "depois de em voo: grava");
    }

    // --- frame VAZIO depois de frame cheio ----------------------------------------------------------
    //
    // Sem candidato, os tres passos por candidato nao despacham; o reset e a finalizacao continuam. O
    // count buffer e os contadores do frame cheio anterior tem de sair zerados.
    {
        CheckEq(G.View.Stats().Outputs, 7u, "vazio: o frame anterior era cheio");
        for (u64 Id : G.IdOpaco) G.Mostrar(Id, false);
        for (u64 Id : G.IdTroca) G.Mostrar(Id, false);
        G.Mostrar(G.IdMascarado, false);
        G.Mostrar(G.IdOpacoB, false);
        const FResultado R = G.Frame(Passes, "vazio");
        CheckEq(R.Gravacao, GpuMainViewOk, "vazio: a cadeia e gravada");
        CheckEq(G.View.Stats().Outputs, 0u, "vazio: nenhum candidato");
        CheckEq(G.UltimaCadeia.Status, 1u, "vazio: o frame fecha");
        for (u32 D : G.UltimaCadeia.Draw) CheckEq(D, 0u, "vazio: o count buffer sai zerado");
        CheckEq(G.View.Layout().TotalCommands(), 6u, "vazio: esconder nao muda o layout");

        for (u64 Id : G.IdOpaco) G.Mostrar(Id, true);
        for (u64 Id : G.IdTroca) G.Mostrar(Id, true);
        G.Mostrar(G.IdMascarado, true);
        G.Mostrar(G.IdOpacoB, true);
        const FResultado R2 = G.Frame(Passes, "cheio de novo");
        CheckEq(R2.Gravacao, GpuMainViewOk, "cheio de novo: a cadeia e gravada");
        CheckEq(G.View.Stats().CandidateRowsUploaded, 7u, "cheio de novo: a lista volta inteira");
    }

    // --- cadeia RECUSADA pela porta da gravacao dos draws -------------------------------------------
    //
    // No mesmo frame, a lista de candidatos muda. A cadeia recusada nao grava nada — a leitura do fim da
    // lista ve a imagem da anterior — e o upload fica pendente: o frame seguinte sobe a lista nova, e a
    // expansao da GPU confere com a CPU.
    {
        const u64 RecusasAntes = G.View.Stats().Refusals[GpuMainViewRecordRefused];
        G.Mostrar(G.IdTroca[0], false);
        const FResultado R = G.Frame(std::span<const FGpuIndirectPassSignature>(G.PasseSemSignature),
                                     "recusada pela gravacao");
        CheckEq(R.Preparo, GpuMainViewOk, "recusada: a preparacao aceita");
        CheckEq(R.Gravacao, GpuMainViewRecordRefused, "recusada: a porta da gravacao dos draws recusa");
        CheckEq(G.View.Stats().LastDetail, GpuIndirectRecordMissingTarget, "recusada: pela signature nula");
        CheckEq64(G.View.Stats().Refusals[GpuMainViewRecordRefused], RecusasAntes + 1u,
                  "recusada: a telemetria conta a recusa");

        const FResultado R2 = G.Frame(Passes, "depois da recusa");
        CheckEq(R2.Gravacao, GpuMainViewOk, "depois da recusa: a cadeia e gravada");
        Check(G.View.Stats().CandidateRowsUploaded > 0u, "depois da recusa: a lista pendente sobe agora");
        CheckEq(G.View.Stats().Outputs, 6u, "depois da recusa: sem o escondido");
        G.Mostrar(G.IdTroca[0], true);
        CheckEq(G.Frame(Passes, "mostrado").Gravacao, GpuMainViewOk, "mostrado: a cadeia e gravada");
    }

    // --- CANARIO na cadeia de verdade: o frame e invalidado inteiro ---------------------------------
    //
    // Um objeto vira RaytracingOnly e so o transform e bumpado. A tabela de instancias republica a linha
    // — o produtor dela recompoe as colunas de quem foi tocado —, mas a lista de candidatos nao e refeita,
    // porque nem estrutura nem static casters mudaram. O candidato chega ao expand, que o recusa e deixa a
    // sentinela, e o classify conta `NoSlot`. Os outros comandos continuam contados: sem os canarios na
    // finalizacao, o frame fecharia desenhando-os, com o objeto sumido. O motivo tem as DUAS marcas, e so
    // a do expand prova que a finalizacao da engine le os contadores dele.
    {
        const u64 Id = G.IdOpaco[2];
        G.R(Id).RaytracingOnly = true;
        G.Scene.BumpTransformsVersion(G.Indice(Id));
        const FResultado R = G.Frame(Passes, "canario");
        CheckEq(R.Gravacao, GpuMainViewOk, "canario: a cadeia e gravada — nenhuma porta da CPU o ve");
        CheckEq(G.UltimaCadeia.Status, 0u, "canario: o frame e invalidado");
        CheckEq(G.UltimaCadeia.Reason, GpuFinalizeClassifyCanary | GpuFinalizeExpandCanary,
                "canario: pelo classify e pelo expand");
        CheckEq(G.UltimaCadeia.Expand[GpuExpandCounterRtOnly], 1u, "canario: o expand recusa o RaytracingOnly");
        CheckEq(G.UltimaCadeia.Classify[GpuClassifyNoSlot], 1u, "canario: o classify ve a sentinela");
        Check(!G.UltimaCadeia.Contagens.empty() && G.UltimaCadeia.Contagens[0] != 0u,
              "canario: os outros comandos continuam contados");
        for (u32 D : G.UltimaCadeia.Draw) CheckEq(D, 0u, "canario: o count buffer sai zerado");

        // Com o bump que faltou, a lista e refeita sem o objeto, e o frame fecha.
        G.Scene.BumpStaticCastersVersion(G.Indice(Id));
        const FResultado R2 = G.Frame(Passes, "canario corrigido");
        CheckEq(R2.Gravacao, GpuMainViewOk, "canario corrigido: a cadeia e gravada");
        CheckEq(G.UltimaCadeia.Status, 1u, "canario corrigido: o frame fecha");
        CheckEq(G.View.Stats().Outputs, 6u, "canario corrigido: o RaytracingOnly saiu da lista");

        G.R(Id).RaytracingOnly = false;
        G.Scene.BumpStaticCastersVersion(G.Indice(Id));
        CheckEq(G.Frame(Passes, "sem RaytracingOnly").Gravacao, GpuMainViewOk, "sem RaytracingOnly: grava");
        CheckEq(G.UltimaCadeia.Status, 1u, "sem RaytracingOnly: o frame fecha");
        CheckEq(G.View.Stats().Outputs, 7u, "sem RaytracingOnly: os sete de volta");
    }

    // --- preparada para uma lista, gravada em outra -------------------------------------------------
    {
        CheckEq(G.View.Prepare(G.Scene, G.Meshes, G.List[0].Get()), GpuMainViewOk,
                "outra lista: a preparacao aceita");
        CheckEq(G.View.Record(G.List[1].Get(), 1u, G.Heap, G.Materials, G.Meshes, G.Instances, Passes),
                GpuMainViewNotPrepared, "outra lista: a gravacao recusa");
        CheckEq(G.View.Record(G.List[0].Get(), 0u, G.Heap, G.Materials, G.Meshes, G.Instances, Passes),
                GpuMainViewNotPrepared, "a preparacao vale para UMA gravacao");
        // As duas gravacoes fora de frame ja coletaram a telemetria dos dois slots.
        for (bool& B : G.CadeiaNoSlot) B = false;
        CheckEq(G.Frame(Passes, "depois de outra lista").Gravacao, GpuMainViewOk,
                "depois de outra lista: o frame seguinte grava");
    }

    // --- capacidade ALOCADA menor que os comandos do layout -----------------------------------------
    //
    // Cinco slots, dois opacos escondidos: cinco candidatos cabem, mas o layout conta os escondidos e tem
    // seis comandos. A porta das regioes recebe a capacidade do RECURSO e recusa antes do dispatch. SEM
    // passe consumidor: a porta da gravacao dos draws tambem veria os argumentos alem do recurso, e com
    // ela no caminho este caso nao provaria a porta das regioes.
    {
        G.Mostrar(G.IdOpaco[0], false);
        G.Mostrar(G.IdOpaco[1], false);
        G.Montar(5u, 2u);
        const FResultado R = G.Frame({}, "capacidade menor");
        CheckEq(R.Preparo, GpuMainViewOk, "capacidade menor: os cinco candidatos cabem");
        CheckEq(R.Gravacao, GpuMainViewRegionsRefused, "capacidade menor: a porta das regioes recusa");
        CheckEq(G.View.Stats().LastDetail, GpuRegionPreflightSmallArgsBuffer,
                "capacidade menor: pelos argumentos alem do recurso");

        G.Mostrar(G.IdOpaco[0], true);
        G.Mostrar(G.IdOpaco[1], true);
        const FResultado R2 = G.Frame(Passes, "sete em cinco slots");
        CheckEq(R2.Preparo, GpuMainViewCandidatesOverflow, "sete em cinco slots: a lista nao cabe");
        CheckEq(R2.Gravacao, GpuMainViewNotPrepared, "sete em cinco slots: nada e gravado");

        G.Montar(G.Instances.Capacity(), 2u);
        CheckEq(G.Frame(Passes, "capacidade da tabela").Gravacao, GpuMainViewOk,
                "com a capacidade da tabela: grava");
    }

    // --- buffers por lote menores que o layout ------------------------------------------------------
    {
        G.Montar(G.Instances.Capacity(), 1u);
        const FResultado R = G.Frame(Passes, "um pool");
        CheckEq(R.Gravacao, GpuMainViewSmallBatchBuffers, "um pool: os lotes do layout nao cabem");
        G.Montar(G.Instances.Capacity(), 2u);
        CheckEq(G.Frame(Passes, "dois pools").Gravacao, GpuMainViewOk, "dois pools: grava");
    }

    // --- a cena CRESCE alem da tabela ---------------------------------------------------------------
    //
    // Sem Setup, nem a publicacao aceita. Com o Setup das tabelas e sem o da cadeia, a lista maior nao
    // cabe nos buffers dela. Com os dois, a cadeia desenha a cena maior.
    {
        u32 Acrescentados = 0;
        while (G.Scene.GpuSlots().Capacity() <= G.Instances.Capacity() && Acrescentados < 100000u) {
            G.Adicionar(&G.MalhaA, &G.Opaco);
            ++Acrescentados;
        }
        FResultado R = G.Frame(Passes, "cena maior");
        CheckEq(R.Publicacao, GpuScenePublicationSceneExceedsTable, "cena maior: sem Setup, nao ha frame");

        G.Instances.SetupForScene(G.Device.Get(), G.Heap, G.Scene);
        R = G.Frame(Passes, "cena maior, tabela refeita");
        CheckEq(R.Preparo, GpuMainViewCandidatesOverflow,
                "cena maior, so a tabela refeita: a lista nao cabe na cadeia");

        G.Montar(G.Instances.Capacity(), 2u);
        R = G.Frame(Passes, "cena maior, cadeia refeita");
        CheckEq(R.Gravacao, GpuMainViewOk, "cena maior, cadeia refeita: grava");
        CheckEq(G.View.Stats().Outputs, 7u + Acrescentados, "cena maior: todos os candidatos");
        CheckEq(G.Frame(Passes, "cena maior, segundo slot").Gravacao, GpuMainViewOk,
                "cena maior: o outro slot tambem grava");
    }

    // --- Release -----------------------------------------------------------------------------------
    G.View.Release(G.Heap);
    CheckEq(G.Heap.UsedSlots(), DescritoresSemCadeia, "o Release devolve todos os descritores da cadeia");
    CheckEq(G.View.Prepare(G.Scene, G.Meshes, G.List[0].Get()), GpuMainViewNotBuilt,
            "depois do Release, a preparacao recusa");
    CheckEq64(G.View.ResidentBytes(), 0ull, "depois do Release, nenhum byte residente");

    Check(G.GravacoesContadas >= 20u, "a contagem de alocacoes cobriu as gravacoes do teste");
    Check(G.TelemetriasConferidas >= 8u, "a telemetria foi conferida em varios frames");
    G.Drenar("fim");

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
} catch (const std::exception& E) {
    std::cerr << "Smile.GpuIndirectMainView: excecao -- " << E.what() << '\n';
    return 2;
}
