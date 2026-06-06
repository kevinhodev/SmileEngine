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
    // Simulacao FFT do oceano (Tessendorf) — agora 100% GPU compute em 256x256
    // (era 64x64 na CPU). O espectro inicial H0 (Phillips com DIRECAO DE VENTO) +
    // amplitudes gaussianas e calculado UMA VEZ na CPU e bakado numa textura; a
    // evolucao temporal H(k,t), a IFFT 2D, o deslocamento, a normal e o Jacobiano
    // (foam) rodam em compute todo frame.
    //
    // Pipeline (espelha o tutorial 56_Ocean do Asylum):
    //   UpdateSpectrum -> FFT(h) -> FFT(D) -> CreateDisplacement -> Gradients -> mips
    //
    // Saidas (contrato IDENTICO ao caminho CPU antigo, so muda a resolucao):
    //   OceanTex  256^2 RGBA32F      = (Dx, Dz, -h, J)        — VS desloca + PS foam/parallax
    //   NormalTex 256^2 RGBA32F+mips = (normal Y-up, ToksvigT) — PS bump de detalhe
    class FOceanFFT {
    public:
        static constexpr u32 kGridSize       = 256; // g_nGridSize
        static constexpr u32 kLogGridSize    = 8;   // log2(256)
        static constexpr u32 kNormalMipCount = 9;   // 256,128,64,32,16,8,4,2,1

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        // Tempo de sim por frame. Interno = 0.125*elapsed (CREWaterOcean::FrameUpdate).
        void SetTime(f32 ElapsedTime) { SimTime = 0.125f * ElapsedTime; }
        // Direcao do vento (rad). Se mudar, re-baka H0 no proximo RecordCompute.
        void SetWindDirection(f32 Rad);
        // Velocidade do vento usada no comprimento dominante do espectro Phillips.
        void SetWindSpeed(f32 V);
        // Amplitude do espectro de Phillips (re-baka H0). Default Cry = 1.0.
        void SetAmplitude(f32 A);

        // Roda o pipeline FFT inteiro na GPU. A command list ja deve ter os descriptor
        // heaps setados (chamada do RenderFrame). Ao final, OceanTex/NormalTex ficam em
        // estado shader-resource (PIXEL|NON_PIXEL), prontos p/ o draw da agua.
        void RecordCompute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        u32  SRVSlot() const       { return OceanSRVSlot; }
        u32  NormalSRVSlot() const { return NormalChainSRVSlot; }
        bool IsInitialized() const { return OceanTex != nullptr && NormalTex != nullptr; }

    private:
        using complexF = std::complex<f32>;
        static constexpr int N = (int)kGridSize;
        static constexpr int M = N + 1; // H0 e (N+1)^2 p/ a simetria conjugada

        // --- Espectro inicial (CPU, 1x ou ao mudar vento/amplitude) ---
        f32  ComputePhillips(f32 kx, f32 ky) const;
        f32  FrandGaussian();
        void ComputeH0();           // preenche H0StagingMapped (respeitando o RowPitch)

        // --- Setup GPU ---
        void CreateTextures(ID3D12Device* Device);
        void CreateDescriptors(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void CreatePipelines(ID3D12Device* Device);
        void TransitionTex(ID3D12GraphicsCommandList* CL, ID3D12Resource* R,
                           D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES Target,
                           u32 Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

        Microsoft::WRL::ComPtr<ID3D12Resource> Create2D(ID3D12Device* Device,
            DXGI_FORMAT Format, u32 Width, u32 Height, u32 Mips, bool AllowUAV,
            D3D12_RESOURCE_STATES InitialState);

        // Constantes da Cry / spectrum.
        f32 Amplitude       = 1.0f;   // m_fA
        f32 WindSpeed       = 4.0f;   // m/s, controla L = V^2/g no Phillips
        f32 WindAngle       = 0.0f;   // direcao do vento (rad) — agora ALIMENTADA pelo usuario
        f32 WorldSize       = 1.0f;   // m_fWorldSizeX/Y
        static constexpr f32 kG = 9.81f;
        f32 MaxWaveSize     = 200.0f; // m_fMaxWaveSize  -> HeightScale no CB
        f32 ChoppyWaveScale = 400.0f; // m_fChoppyWaveScale -> ChoppyScale no CB
        f32 NormalUp        = 8.0f;   // componente "up" da normal (WaterVolumesNormalGenPS)
        f32 ChoppyJacobianScale = 0.15f; // sensibilidade do foam (ver doc no antigo header)
        f32 SimTime         = 0.0f;

        // RNG deterministico p/ H0 (Box-Muller). Re-semeado a cada ComputeH0.
        std::mt19937 Rng{ 1337u };
        bool GaussianHasLast = false;
        f32  GaussianLast    = 0.0f;

        // CB compartilhado pelos passos (b0). 256 bytes p/ CBV.
        struct alignas(256) OceanCB {
            f32 Time;
            f32 ChoppyScale;
            f32 HeightScale;
            f32 NormalUp;
            f32 JacobianScale;
            f32 _Pad[3];
        };
        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        OceanCB* MappedCB = nullptr;

        // --- Texturas GPU ---
        Microsoft::WRL::ComPtr<ID3D12Resource> H0Tex;       // (N+1)^2 RGBA32F (h0.xy, omega, 0)
        Microsoft::WRL::ComPtr<ID3D12Resource> H0Staging;   // UPLOAD mapeado
        u8*  H0StagingMapped = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT H0Footprint{};
        bool H0Dirty = true;

        Microsoft::WRL::ComPtr<ID3D12Resource> SpecH;       // N^2 RG32F
        Microsoft::WRL::ComPtr<ID3D12Resource> SpecD;       // N^2 RG32F
        Microsoft::WRL::ComPtr<ID3D12Resource> FFTTemp;     // N^2 RG32F (scratch transpose)
        Microsoft::WRL::ComPtr<ID3D12Resource> DispTex;     // N^2 RGBA32F (Dx,Dz,-h,0)
        Microsoft::WRL::ComPtr<ID3D12Resource> OceanTex;    // N^2 RGBA32F (Dx,Dz,-h,J)
        Microsoft::WRL::ComPtr<ID3D12Resource> NormalTex;   // N^2 RGBA32F + mips

        // Estados (single-subresource exceto NormalTex, que e por-mip).
        D3D12_RESOURCE_STATES H0State      = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_RESOURCE_STATES SpecHState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES SpecDState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES FFTTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES DispState    = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES OceanState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES NormalMipState[kNormalMipCount];

        // --- Descritores (no heap compartilhado) ---
        u32 H0SRVSlot          = 0;             // t0 do UpdateSpectrum
        u32 SpecSRVPair        = 0;             // [SpecH_srv, SpecD_srv] (createdisp + fft)
        u32 FFTTempSRVSlot     = 0;
        u32 SpecUAVPair        = 0;             // [SpecH_uav, SpecD_uav] (updatespectrum + fft)
        u32 FFTTempUAVSlot     = 0;
        u32 DispSRVSlot        = 0;
        u32 DispUAVSlot        = 0;
        u32 GradUAVPair        = 0;             // [OceanTex_uav, NormalTex_mip0_uav]
        u32 OceanSRVSlot       = 0;             // OceanTex full (consumido pela agua)
        u32 NormalChainSRVSlot = 0;             // NormalTex chain completa (PS)
        u32 NormalMipUAVSlot[kNormalMipCount]{}; // UAV por mip (mip0 = GradUAVPair+1)
        u32 NormalMipSRVSlot[kNormalMipCount]{}; // SRV single-mip dos mips de origem (0..N-2)

        // --- Pipelines de compute ---
        FVolumetricPipeline UpdateSpectrumPSO; // 1 SRV, 2 UAV
        FVolumetricPipeline FFTPSO;            // 1 SRV, 1 UAV
        FVolumetricPipeline CreateDispPSO;     // 2 SRV, 1 UAV
        FVolumetricPipeline GradientsPSO;      // 1 SRV, 2 UAV
        FVolumetricPipeline NormalMipPSO;      // 1 SRV, 1 UAV
    };
}
