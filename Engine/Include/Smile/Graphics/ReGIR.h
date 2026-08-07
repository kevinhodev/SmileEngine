#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Contrato publicado nos CBs dos consumidores de HitShading.hlsli. Os indices apontam para
    // descritores bindless estaveis; so a paridade corrente muda de um frame para o outro.
    struct FReGIRShaderParams {
        Vec4 GridMinSlots;       // xyz = minimo mundo; w = slots por celula
        Vec4 InvCellSizeEnabled; // xyz = 1/tamanho da celula; w = 0/1
        Vec4 GridCountSamples;   // xyz = dimensoes; w = candidatas no segundo RIS
        Vec4 Resources;          // x = SRV dos slots; y = SRV da media por celula; zw livres
    };

    // Um slot de ReGIR e um reservoir de UMA luz. Mantido em 16 bytes para que o grid
    // 16x8x16 com 64 slots/celula ocupe 2 MiB por paridade (4 MiB no ping-pong).
    struct FReGIRReservoirGPU {
        u32 LightIndex;
        f32 AverageWeight;
        f32 SampleTarget;
        u32 HistoryLength;
    };
    static_assert(sizeof(FReGIRReservoirGPU) == 16);

    struct alignas(256) ReGIRConstants {
        Vec4 GridMinSlots;    // xyz = minimo mundo; w = slots/celula
        Vec4 CellSizeHistory; // xyz = tamanho da celula; w = maximo de frames no reservoir
        Vec4 GridCountLights; // xyz = dimensoes; w = numero de luzes
        Vec4 BuildParams;     // x = frame; y = historico valido; z = candidatas iniciais; w livre
    };

    // ReGIR (Ray Tracing Gems II, cap. 23): pools de reservoirs em uma grade de mundo para
    // selecionar luz local em pontos arbitrarios. A superficie primaria continua no ReSTIR DI;
    // este grid existe para os hits secundarios compartilhados por DDGI, reflexoes e ReSTIR GI.
    class FReGIR : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "ReGIR (build)"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        static constexpr u32 kGridX = 16;
        static constexpr u32 kGridY = 8;
        static constexpr u32 kGridZ = 16;
        static constexpr u32 kSlotsPerCell = 64;
        static constexpr u32 kInitialCandidates = 8;
        static constexpr u32 kShadingCandidates = 8;
        static constexpr u32 kMaxHistory = 8;

        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device);

        // O prototipo estica a grade pela AABB da cena, como a opcao simples do capitulo.
        // Os SRVs de luz sao uma fatia por frame em voo e ficam imutaveis nas tabelas.
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                           const Vec3& AABBMin, const Vec3& AABBMax,
                           const u32 LightSlots[FCommandQueue::kFramesInFlight]);

        void UpdatePerFrame(u32 FrameSlot, u32 FrameIndex, u32 LightCount, u64 LightSetSignature);
        void RecordBuild(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        void InvalidateHistory() { HistoryValid = false; }
        bool IsReady() const { return Ready; }
        bool HasOutput() const { return Ready && HasCurrent; }
        FReGIRShaderParams ShaderParams(bool Enabled) const;

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseScene(FTextureSRVHeap& SRVHeap);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FVolumetricPipeline BuildPSO;   // t0 prev slots, t1 luzes; u0 curr slots
        FVolumetricPipeline AveragePSO; // t0 curr slots; u0 media por celula

        Microsoft::WRL::ComPtr<ID3D12Resource> Slots[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> CellAverage[2];
        D3D12_RESOURCE_STATES SlotsState[2] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES AverageState[2] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 SlotsSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 SlotsUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 AverageSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 AverageUAV[2] = { kInvalidSlot, kInvalidSlot };
        // [paridade de escrita][frame em voo]. t0 e fixo pela paridade; t1 varia pela fatia
        // do upload de luzes. Tudo e montado no setup, sem descriptor rewrite em voo.
        u32 BuildTable[2][FCommandQueue::kFramesInFlight] = {
            { kInvalidSlot, kInvalidSlot }, { kInvalidSlot, kInvalidSlot } };
        static_assert(FCommandQueue::kFramesInFlight == 2,
                      "ajustar inicializacao/versionamento das tabelas do ReGIR");

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;
        ReGIRConstants CPU{};
        u32 FrameSlot = 0;

        Vec3 GridMin{};
        Vec3 CellSize{ 1.0f, 1.0f, 1.0f };
        u32 WriteParity = 0;
        u32 CurrentParity = 0;
        u64 LastLightSetSignature = 0;
        bool HistoryValid = false;
        bool HasCurrent = false;
        bool Initialized = false;
        bool Ready = false;
    };
}
