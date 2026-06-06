#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Vec4.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <complex>
#include <random>

namespace Smile {
    // Simulacao FFT do oceano (Tessendorf) na CPU — PORT VERBATIM do CWaterSim da
    // CryEngine (RenderDll/Common/WaterUtils.cpp). Grid 64x64, espectro de Phillips,
    // amplitudes H0 gaussianas (Box-Muller), evolucao temporal H(k,t) + espelho
    // Hermitiano (so metade do grid e calculada), IFFT 2D (Cooley-Tukey por linhas e
    // colunas) e empacotamento (Dx, Dy, -h, 0).
    //
    // Quirks da Cry preservados (fieis a release):
    //  - vento da FFT FIXO: angulo ignorado, vetor |w|=1 (Create forca m_fWind=0), entao
    //    L = |w|^2/g = 1/g (nao zero). O vento real so rola o bump no shader (OceanParams1.yz).
    //  - worldSize = 1 -> k = (i - N/2) * 2*PI.
    //  - IFFT (Dir=-1) SEM normalizar por N (so o forward Dir=+1 divide por N). As escalas
    //    finais MaxWaveSize=200 / ChoppyWaveScale=400 compensam.
    //  - tempo de sim = 0.125 * elapsed (CREWaterOcean::FrameUpdate:346).
    //
    // O resto do sistema so consome a textura RGBA32F 64x64 = (Dx, Dy, -h, 0).
    class FOceanFFT {
    public:
        static constexpr u32 kGridSize    = 64; // g_nGridSize
        static constexpr u32 kLogGridSize = 6;  // log2(64)

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        // Roda a sim (tempo interno = 0.125*ElapsedTime) e escreve o buffer de staging.
        void Update(f32 ElapsedTime);
        // Copia staging -> textura GPU. Chamar com a command list do frame ja aberta
        // (apos SetDescriptorHeaps, antes do draw da agua).
        void RecordUpload(ID3D12GraphicsCommandList* CommandList);

        u32  SRVSlot() const     { return TexSRVSlot; }
        u32  NormalSRVSlot() const { return NormalTexSRVSlot; }
        bool IsInitialized() const { return OceanTex != nullptr && NormalTex != nullptr; }

        // Amplitude do espectro de Phillips (re-gera H0). Default Cry = 1.0.
        void SetAmplitude(f32 A) { Amplitude = A; InitFourierAmps(); }

    private:
        using complexF = std::complex<f32>;
        static constexpr int N = (int)kGridSize;

        // --- Simulacao (porte do CWaterSim) ---
        void InitTableK();
        void InitFourierAmps();
        void UpdateSim(f32 Time);
        void BuildNormalMipChain();
        void StageDisplacement();
        void StageNormalMipChain();
        void ComputeFFT1D(int Dir, f32* Real, f32* Imag);
        void ComputeFFT2D(int Dir, complexF* C);
        f32  ComputePhillips(f32 kx, f32 ky) const;
        f32  FrandGaussian();

        static int PowNeg1(int n) { return (n & 1) ? -1 : 1; }
        int Offset(int x, int y) const { return y * N + x; }
        int OffsetMirror(int x, int y) const {
            return Offset((N - x) & (N - 1), (N - y) & (N - 1));
        }
        static u32 NormalMipSize(u32 Mip);
        static u32 NormalMipOffset(u32 Mip);

        // Buffers da sim (fixos, como na Cry).
        Vec4     LUTK[kGridSize * kGridSize];        // (kx, ky, |k|, omega)
        complexF FourierAmps[kGridSize * kGridSize]; // H0
        complexF HeightField[kGridSize * kGridSize];
        complexF DisplaceX[kGridSize * kGridSize];
        complexF DisplaceY[kGridSize * kGridSize];
        Vec4     DisplaceGrid[kGridSize * kGridSize]; // (Dx, Dy, -h, 0)
        // Normal/height texture derivada do FFT, equivalente ao s_ptexWaterVolumeDDN
        // da Cry. Mip .a guarda Toksvig T para anti-aliasing especular no PS.
        static constexpr u32 kNormalMipCount = 7; // 64,32,16,8,4,2,1
        static constexpr u32 kNormalMipTexelCount =
            64u * 64u + 32u * 32u + 16u * 16u + 8u * 8u + 4u * 4u + 2u * 2u + 1u;
        Vec4 NormalMipChain[kNormalMipTexelCount];

        // Constantes da Cry.
        f32 Amplitude       = 1.0f;   // m_fA
        f32 WindAngle       = 0.0f;   // m_fWind (forcado 0)
        f32 WorldSize       = 1.0f;   // m_fWorldSizeX/Y
        static constexpr f32 kG = 9.81f;
        f32 MaxWaveSize     = 200.0f; // m_fMaxWaveSize
        f32 ChoppyWaveScale = 400.0f; // m_fChoppyWaveScale

        // RNG determinístico p/ H0 (Box-Muller).
        std::mt19937 Rng{ 1337u };
        bool GaussianHasLast = false;
        f32  GaussianLast    = 0.0f;

        // --- GPU ---
        Microsoft::WRL::ComPtr<ID3D12Resource> OceanTex;   // DEFAULT 64x64 RGBA32F
        Microsoft::WRL::ComPtr<ID3D12Resource> StagingBuf; // UPLOAD (mapeado persistente)
        u8* StagingMapped = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        u32 TexSRVSlot = 0;
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;

        Microsoft::WRL::ComPtr<ID3D12Resource> NormalTex;   // DEFAULT 64x64 RGBA32F + mips
        Microsoft::WRL::ComPtr<ID3D12Resource> NormalStagingBuf;
        u8* NormalStagingMapped = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT NormalFootprints[kNormalMipCount]{};
        UINT   NormalNumRows[kNormalMipCount]{};
        UINT64 NormalRowSize[kNormalMipCount]{};
        UINT64 NormalTotalSize = 0;
        u32 NormalTexSRVSlot = 0;
        D3D12_RESOURCE_STATES NormalCurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
    };
}
