#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Scene/CapturedIdentity.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Occlusion culling hierarquico (HZB, estilo UE "furthest HZB" + consumo assincrono
    // estilo coverage buffer da Cry). Fluxo por frame N:
    //   1. RecordBuild — cadeia de mips do depth do prepass (min-reduce em reverse-Z);
    //   2. RecordTest  — 1 thread/AABB contra o HZB com a VP SEM jitter do proprio N,
    //      resultado copiado pro slot N do readback ring (kFramesInFlight);
    //   3. frame N+2: BeginFrame(slot) — a fence do slot ja foi esperada no
    //      BeginFrame, a CPU le sem stall e o Renderer descarta ocludidos do draw list.
    // Conservador: fora do frustum/cruzando near/entrada nula/frame invalido = visivel.
    //
    // Mip 0 = metade da resolucao de render arredondada PRA CIMA em potencia de 2
    // (igual UE): mips halvam exato ate 1x1, sem o problema de borda impar. A area
    // de padding alem da tela fica em far (0) — conservadora por construcao.
    class FHiZOcclusion : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "HZB (occlusion culling)"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;
        EHistoryTarget HistoryTargets() const override { return EHistoryTarget::HiZOcclusion; }
        void OnInvalidateHistory(EHistoryTarget) override { InvalidateResults(); }

        // SetupObjects pode rodar ANTES do Initialize (RecreateObjectCB no boot do
        // Renderer) — os arrays de slots ja nascem invalidos aqui no construtor.
        FHiZOcclusion() {
            for (u32 i = 0; i < kMaxMips; ++i) {
                MipSRVSlot[i] = kInvalidSlot;
                MipUAVSlot[i] = kInvalidSlot;
            }
            for (u32 s = 0; s < kSlots; ++s) TestTableSlot[s] = kInvalidSlot;
        }

        void Initialize(ID3D12Device* Device);
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                            u32 RenderW, u32 RenderH);

        // Buffers de bounds/resultado/readback p/ ate MaxObjects AABBs. Chamado do
        // RecreateObjectCB (init + crescimento no load de cena); independe do
        // Initialize/SetupForResize — os descritores cruzados sao refeitos quando
        // as duas metades existem.
        void SetupObjects(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 MaxObjects);

        // Liga a coluna canonica de bounds por slot. O descritor e copiado para as tabelas do
        // passe; nenhum bound volta a ser duplicado no upload do HiZ.
        void SetInstanceBounds(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               u32 BoundsSRV, u32 BoundsCapacity);

        // Grava a construcao da cadeia. Contrato: DepthBuffer ja em
        // NON_PIXEL_SHADER_RESOURCE; ao final o HZB inteiro fica em
        // NON_PIXEL_SHADER_RESOURCE (pronto pro teste/leitura).
        void RecordBuild(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                         u32 DepthSRVSlot);

        // Depois da fence do frame slot: consome o remap antigo ANTES de reutiliza-lo.
        void BeginFrame(u32 FrameSlot, const FGpuSceneSlots& Slots);
        // Dispatch compacto: a ordem desta captura pertence ao teste, nunca a cena.
        void WriteCandidate(u32 FrameSlot, FGpuSceneHandle Handle);

        // Teste + copia pro readback do slot. Chamar DEPOIS do RecordBuild do mesmo
        // frame; ViewProj SEM jitter. Marca o slot como valido p/ o resolve futuro.
        void RecordTest(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                        u32 FrameSlot, const Mat44& ViewProjNoJitter);

        // Sem resultado DAQUELE handle na amostra consumida, usa visivel.
        bool IsVisible(FGpuSceneHandle Handle) const { return Visibility.IsVisible(Handle); }

        // Descarta os resultados pendentes (toggle re-ligado, cena trocada, teleporte).
        void InvalidateResults();

        bool IsReady() const { return Ready; }
        bool ObjectsReady() const { return ObjectCapacity > 0; }
        u32  Capacity() const { return ObjectCapacity; } // resultado so cobre [0, cap)
        u32  ChainSRVSlot() const { return FullSRVSlot; } // cadeia inteira (teste/debug)
        u32  MipCount() const { return NumMips; }
        u32  Mip0Width()  const { return HZBWidth; }
        u32  Mip0Height() const { return HZBHeight; }

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void ReleaseSizedResources(FTextureSRVHeap& SRVHeap);
        void ReleaseObjectResources(FTextureSRVHeap& SRVHeap);
        void RefreshTestTables(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        static constexpr u32 kMaxMips     = 14; // 8192 po2 no mip 0
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        static constexpr u32 kSlots       = FCommandQueue::kFramesInFlight;

        FComputePipeline    BuildPSO;
        FComputePipeline TestPSO; // b0 CB + t0 slots/t1 bounds/t2 HZB + u0 visibilidade
        Microsoft::WRL::ComPtr<ID3D12Resource> HZB;

        u32 HZBWidth  = 0;   // dimensoes do mip 0 (po2)
        u32 HZBHeight = 0;
        u32 SrcWidth  = 0;   // resolucao de render (area valida do depth)
        u32 SrcHeight = 0;
        u32 NumMips   = 0;

        u32 MipSRVSlot[kMaxMips]; // SRV de um mip so (fonte da reducao seguinte)
        u32 MipUAVSlot[kMaxMips];
        u32 FullSRVSlot = kInvalidSlot;

        // --- teste + readback ---
        struct FTestConstants {
            Mat44 ViewProj;
            f32   RenderW, RenderH;
            u32   ObjectCount, NumMips;
            f32   DepthBias;
            u32   InstanceCapacity;
            f32   Pad[2];
        };
        struct FSlotMeta {
            std::vector<FGpuSceneHandle> Handles;
            u32 Count = 0;
            bool Valid = false;
        };
        FCapturedVisibility Visibility;

        Microsoft::WRL::ComPtr<ID3D12Resource> CandidateBuffer; // u32 slot, upload ring kSlots
        Microsoft::WRL::ComPtr<ID3D12Resource> ResultBuffer;   // default, UAV
        Microsoft::WRL::ComPtr<ID3D12Resource> ReadbackBuffer; // readback ring kSlots
        Microsoft::WRL::ComPtr<ID3D12Resource> TestCB;         // upload ring kSlots

        u32*       MappedCandidates = nullptr;
        const u32* MappedReadback = nullptr;
        u8*        MappedTestCB   = nullptr;

        u32 ObjectCapacity = 0;
        u32 TestTableSlot[kSlots]; // 3 descritores: [slots, bounds canonicos, cadeia HZB]
        u32 ResultUAVSlot = kInvalidSlot;
        D3D12_RESOURCE_STATES ResultState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        FSlotMeta SlotMeta[kSlots];

        u32 InstanceBoundsSRV_      = kInvalidSlot;
        u32 InstanceBoundsCapacity_ = 0;

        bool Initialized = false;
        bool Ready       = false;
    };
}
