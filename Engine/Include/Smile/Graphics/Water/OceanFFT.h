#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Vec4.h"
#include "Smile/Graphics/RHI/CommandQueue.h"
#include "Smile/Graphics/RHI/TextureSRVHeap.h"
#include "Smile/Graphics/RHI/ComputePipeline.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <random>

namespace Smile {
    class FOceanFFT : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Água — FFT"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;
        EHistoryTarget HistoryTargets() const override { return EHistoryTarget::OceanTemporal; }
        void OnInvalidateHistory(EHistoryTarget) override { ResetTemporalHistory(); }

        static constexpr u32 kGridSize       = 256;
        static constexpr u32 kLogGridSize    = 8;
        static constexpr u32 kDisplacementMipCount = 9;
        static constexpr u32 kNormalMipCount = 9;

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void RecreatePipelines(ID3D12Device* Device);

        void SetTime(f32 ElapsedTime) { SimTime = ElapsedTime; RealTime = ElapsedTime; }
        void SetWindDirection(f32 Rad);
        void SetWindSpeed(f32 V);
        void SetAmplitude(f32 A);
        void SetSpectrumFetch(f32 Kilometres);
        void SetOceanDepth(f32 Metres);
        void SetSwell(f32 Value);

        // Config por cascata: domínio periódico real em metros e banda em ciclos/tile
        // [Low, High). A dispersão usa k=2*pi*ciclos/WorldSize, sem escala temporal.
        void ConfigureCascade(u32 SeedValue, f32 WorldSizeMeters,
                              f32 CyclesLow, f32 CyclesHigh);

        // Applied exactly once to the final displacement field. Normals and the
        // Jacobian are then derived from that same scaled surface.
        void SetGeometryScales(f32 HeightScale, f32 ChoppyLambda);
        void SetFoamRecovery(f32 PerSecond) { FoamRecovery = PerSecond; }

        void RecordCompute(u32 FrameSlot, ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        u32  SRVSlot() const {
            return OceanSRVSlot[CurrentIndex];
        }
        u32  PreviousSRVSlot() const {
            return PreviousSampleValid
                ? OceanSRVSlot[1u - CurrentIndex]
                : OceanSRVSlot[CurrentIndex];
        }
        u32  NormalSRVSlot() const { return NormalChainSRVSlot; }
        bool IsInitialized() const override {
            return OceanTex[0] != nullptr && OceanTex[1] != nullptr && NormalTex != nullptr;
        }
        void ResetTemporalHistory() {
            DisplacementHistoryValid = false;
            PreviousSampleValid = false;
            FoamHistoryValid = false;
        }

    private:
        static constexpr int N = static_cast<int>(kGridSize);
        f32  FrandGaussian();
        void ComputeH0(u32 StagingSlot);

        void CreateTextures(ID3D12Device* Device);
        void CreateDescriptors(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void CreatePipelines(ID3D12Device* Device);
        void TransitionTex(ID3D12GraphicsCommandList* CL, ID3D12Resource* R,
                           D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES Target,
                           u32 Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

        Microsoft::WRL::ComPtr<ID3D12Resource> Create2D(ID3D12Device* Device,
            DXGI_FORMAT Format, u32 Width, u32 Height, u32 Mips, bool AllowUAV,
            D3D12_RESOURCE_STATES InitialState);

        f32 Amplitude       = 1.0f;
        f32 WindSpeed       = 4.0f;
        f32 WindAngle       = 0.0f;
        f32 FetchKilometres = 100.0f;
        f32 OceanDepth      = 100.0f;
        f32 Swell           = 0.25f;
        f32 WorldSize       = 64.0f;
        f32 SurfaceHeightScale = 1.0f;
        f32 ChoppyLambda       = 1.0f;
        f32 SimTime         = 0.0f;
        f32 RealTime        = 0.0f;
        f32 LastRealTime    = 0.0f;
        f32 FoamRecovery    = 0.18f; // J recupera 0.18/s → espuma some em ~3-4 s
        bool FoamHistoryValid = false;

        std::mt19937 Rng{ 1337u };
        u32  Seed             = 1337u;
        f32  CutoffLowCycles  = 0.0f;    // banda do espectro em ciclos/tile [Low, High)
        f32  CutoffHighCycles = 1.0e9f;
        bool GaussianHasLast = false;
        f32  GaussianLast    = 0.0f;

        struct alignas(256) OceanCB {
            f32 Time;
            f32 ChoppyScale;
            f32 HeightScale;
            f32 InvTwoTexelWorld;
            f32 DeltaTime;
            f32 FoamRecovery;
            f32 FoamReset;
            f32 Padding0;
            f32 WindDirectionX;
            f32 WindDirectionZ;
            f32 Padding1;
            f32 Padding2;
        };
        static_assert(sizeof(OceanCB) == 256, "OceanCB must keep the D3D12 CBV stride");
        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8*      MappedCBBase = nullptr;
        OceanCB* MappedCB     = nullptr;
        u32      FrameSlot    = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> H0Tex;
        // Um upload por frame: BeginFrame espera a fence do slot antes de RecordCompute
        // escreve nele, portanto o CopyTextureRegion nunca le memoria sendo sobrescrita.
        Microsoft::WRL::ComPtr<ID3D12Resource> H0Staging[FCommandQueue::kFramesInFlight];
        u8* H0StagingMapped[FCommandQueue::kFramesInFlight]{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT H0Footprint{};
        bool H0Dirty = true;

        Microsoft::WRL::ComPtr<ID3D12Resource> SpecHD;
        Microsoft::WRL::ComPtr<ID3D12Resource> FFTTemp;
        Microsoft::WRL::ComPtr<ID3D12Resource> DispTex;
        // Ping-pong: o frame N escreve OceanTex[Write] e o VS le o N-1 em OceanTex[1-Write].
        // Elimina o CopyResource da cadeia inteira (9 mips RGBA32F) por cascata, por frame.
        Microsoft::WRL::ComPtr<ID3D12Resource> OceanTex[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> NormalTex;
        u32 CurrentIndex = 0;

        D3D12_RESOURCE_STATES H0State      = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_RESOURCE_STATES SpecHDState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES FFTTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES DispState    = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES OceanMipState[2][kDisplacementMipCount]{};
        D3D12_RESOURCE_STATES NormalMipState[kNormalMipCount]{};
        bool DisplacementHistoryValid = false;
        bool PreviousSampleValid = false;

        u32 H0SRVSlot          = 0;
        u32 SpecSRVSlot        = 0;
        u32 FFTTempSRVSlot     = 0;
        u32 SpecUAVSlot        = 0;
        u32 FFTTempUAVSlot     = 0;
        u32 DispSRVSlot        = 0;
        u32 DispUAVSlot        = 0;
        u32 GradUAVPair[2]     = {};
        u32 GradSRVPair[2]     = {};
        u32 GradSRVDummy       = 0;
        u32 OceanSRVSlot[2]    = {};
        u32 OceanMipUAVSlot[2][kDisplacementMipCount]{};
        u32 OceanMipSRVSlot[2][kDisplacementMipCount - 1]{};
        u32 NormalChainSRVSlot = 0;
        u32 NormalMipUAVSlot[kNormalMipCount]{};
        u32 NormalMipSRVSlot[kNormalMipCount]{};

        FComputePipeline UpdateSpectrumPSO;
        FComputePipeline FFTPSO;
        FComputePipeline CreateDispPSO;
        FComputePipeline GradientsPSO;
        FComputePipeline DisplacementMipPSO;
        FComputePipeline NormalMipPSO;
    };
}
