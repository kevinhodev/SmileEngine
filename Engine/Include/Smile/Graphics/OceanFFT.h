#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Vec4.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <complex>
#include <random>
#include <vector>

namespace Smile {
    class FOceanFFT {
    public:
        static constexpr u32 kGridSize       = 256;
        static constexpr u32 kLogGridSize    = 8;
        static constexpr u32 kNormalMipCount = 9;

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        void SetTime(f32 ElapsedTime) { SimTime = 0.125f * TimeFactor * ElapsedTime; RealTime = ElapsedTime; }
        void SetWindDirection(f32 Rad);
        void SetWindSpeed(f32 V);
        void SetAmplitude(f32 A);

        // Config por cascata (multi-cascata): seed própria (padrões descorrelacionados),
        // fator de tempo (dispersão física: cascata maior evolui ~1/sqrt(T_i/T_0) mais
        // devagar) e banda do espectro em CICLOS por tile [Low, High) — bandas disjuntas
        // entre cascatas evitam energia duplicada na soma. Chamar ANTES do Initialize
        // (ou seguido de re-bake via setters de vento/amplitude).
        void ConfigureCascade(u32 SeedValue, f32 TimeScale, f32 CyclesLow, f32 CyclesHigh) {
            Seed = SeedValue; TimeFactor = TimeScale;
            CutoffLowCycles = CyclesLow; CutoffHighCycles = CyclesHigh;
        }

        // Acopla o Jacobiano (espuma) ao choppy EFETIVO da superfície: recebe o produto
        // dos sliders (choppy × dispScale × wavesSize × wavesAmount); a calibração
        // preserva o look validado nos defaults (0.15 em 1.5×1.0×0.75×1.5).
        void SetChoppyFactors(f32 SliderProduct) {
            ChoppyJacobianScale = kChoppyJacobianCalib * (SliderProduct < 0.0f ? 0.0f : SliderProduct);
        }
        void SetFoamRecovery(f32 PerSecond) { FoamRecovery = PerSecond; }

        void RecordCompute(u32 FrameSlot, ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        u32  SRVSlot() const       { return OceanSRVSlot; }
        u32  NormalSRVSlot() const { return NormalChainSRVSlot; }
        bool IsInitialized() const { return OceanTex != nullptr && NormalTex != nullptr; }

    private:
        using complexF = std::complex<f32>;
        static constexpr int N = static_cast<int>(kGridSize);
        static constexpr int M = N + 1; 

        f32  ComputePhillips(f32 kx, f32 ky) const;
        f32  FrandGaussian();
        void ComputeH0();

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
        f32 WorldSize       = 1.0f;
        static constexpr f32 kG = 9.81f;
        f32 MaxWaveSize     = 200.0f;
        f32 ChoppyWaveScale = 400.0f;
        f32 NormalUp        = 8.0f;
        static constexpr f32 kChoppyJacobianCalib = 0.15f / (1.5f * 1.0f * 0.75f * 1.5f);
        f32 ChoppyJacobianScale = 0.15f;
        f32 SimTime         = 0.0f;
        f32 RealTime        = 0.0f;
        f32 LastRealTime    = 0.0f;
        f32 FoamRecovery    = 0.18f; // J recupera 0.18/s → espuma some em ~3-4 s
        bool FoamHistoryValid = false;

        std::mt19937 Rng{ 1337u };
        u32  Seed             = 1337u;
        f32  TimeFactor       = 1.0f;
        f32  CutoffLowCycles  = 0.0f;    // banda do espectro em ciclos/tile [Low, High)
        f32  CutoffHighCycles = 1.0e9f;
        bool GaussianHasLast = false;
        f32  GaussianLast    = 0.0f;

        struct alignas(256) OceanCB {
            f32 Time;
            f32 ChoppyScale;
            f32 HeightScale;
            f32 NormalUp;
            f32 JacobianScale;
            f32 DeltaTime;
            f32 FoamRecovery;
            f32 FoamReset;
        };
        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8*      MappedCBBase = nullptr;
        OceanCB* MappedCB     = nullptr;
        u32      FrameSlot    = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> H0Tex;
        Microsoft::WRL::ComPtr<ID3D12Resource> H0Staging;
        u8* H0StagingMapped = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT H0Footprint{};
        bool H0Dirty = true;

        Microsoft::WRL::ComPtr<ID3D12Resource> SpecH;
        Microsoft::WRL::ComPtr<ID3D12Resource> SpecD;
        Microsoft::WRL::ComPtr<ID3D12Resource> FFTTemp;
        Microsoft::WRL::ComPtr<ID3D12Resource> DispTex;
        Microsoft::WRL::ComPtr<ID3D12Resource> OceanTex;
        Microsoft::WRL::ComPtr<ID3D12Resource> NormalTex;

        D3D12_RESOURCE_STATES H0State      = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_RESOURCE_STATES SpecHState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES SpecDState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES FFTTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES DispState    = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES OceanState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES NormalMipState[kNormalMipCount]{};

        u32 H0SRVSlot          = 0;
        u32 SpecSRVPair        = 0;
        u32 FFTTempSRVSlot     = 0;
        u32 SpecUAVPair        = 0;
        u32 FFTTempUAVSlot     = 0;
        u32 DispSRVSlot        = 0;
        u32 DispUAVSlot        = 0;
        u32 GradUAVPair        = 0;
        u32 OceanSRVSlot       = 0;
        u32 NormalChainSRVSlot = 0;
        u32 NormalMipUAVSlot[kNormalMipCount]{};
        u32 NormalMipSRVSlot[kNormalMipCount]{};

        FVolumetricPipeline UpdateSpectrumPSO;
        FVolumetricPipeline FFTPSO;
        FVolumetricPipeline CreateDispPSO;
        FVolumetricPipeline GradientsPSO;
        FVolumetricPipeline NormalMipPSO;
    };
}
