#include "Smile/Graphics/OceanFFT.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Smile {

    // =====================================================================================
    // Espectro inicial H0 (CPU, 1x ou ao mudar vento/amplitude)
    // =====================================================================================

    f32 FOceanFFT::FrandGaussian() {
        if (GaussianHasLast) {
            GaussianHasLast = false;
            return GaussianLast;
        }
        std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);
        f32 x1, x2, w;
        do {
            x1 = dist(Rng);
            x2 = dist(Rng);
            w = x1 * x1 + x2 * x2;
        } while (w >= 1.0f);
        w = std::sqrt((-2.0f * std::log(w)) / w);
        GaussianLast    = x2 * w;
        GaussianHasLast = true;
        return x1 * w;
    }

    // Phillips. pW = -(cos(wind), sin(wind)); o vento vem do usuario, com damping
    // suave contra o vento para direcionalidade.
    f32 FOceanFFT::ComputePhillips(f32 kx, f32 ky) const {
        const f32 k2 = kx * kx + ky * ky;
        if (k2 == 0.0f) return 0.0f;

        const f32 wx = -std::cos(WindAngle);
        const f32 wy = -std::sin(WindAngle);
        const f32 w2 = wx * wx + wy * wy;
        const f32 Wind = std::max(WindSpeed, 0.1f);
        const f32 L  = (Wind * Wind) / kG;
        const f32 L2 = L * L;
        const f32 kDotW = kx * wx + ky * wy;

        f32 P = Amplitude *
                (std::exp(-1.0f / (k2 * L2)) / (k2 * k2)) *
                (kDotW * kDotW / std::max(k2 * w2, 1e-6f));
        if (kDotW < 0.0f) P *= 0.25f;
        return P;
    }

    // Preenche o staging de H0: (h0(k).x, h0(k).y, omega(k), 0) sobre (N+1)^2.
    void FOceanFFT::ComputeH0() {
        if (!H0StagingMapped) return;

        const f32 kTwoPi       = 6.28318530717958647692f;
        const f32 recipSqrt2   = 1.0f / std::sqrt(2.0f);
        const f32 pi2OverWorld = kTwoPi / std::max(WorldSize, 1e-3f);
        const f32 start        = N / 2.0f;
        const UINT rowPitch    = H0Footprint.Footprint.RowPitch;

        Rng.seed(1337u);
        GaussianHasLast = false;

        for (int m = 0; m < M; ++m) {
            const f32 ky = (start - static_cast<f32>(m)) * pi2OverWorld;
            Vec4* row = reinterpret_cast<Vec4*>(
                H0StagingMapped + H0Footprint.Offset + static_cast<UINT64>(m) * rowPitch);

            for (int n = 0; n < M; ++n) {
                const f32 kx = (start - static_cast<f32>(n)) * pi2OverWorld;

                f32 sqrtP = 0.0f;
                if (kx != 0.0f || ky != 0.0f) {
                    const f32 P = ComputePhillips(kx, ky);
                    sqrtP = (P > 0.0f) ? std::sqrt(P) : 0.0f;
                }

                const f32 h0x   = sqrtP * FrandGaussian() * recipSqrt2;
                const f32 h0y   = sqrtP * FrandGaussian() * recipSqrt2;
                const f32 klen  = std::sqrt(kx * kx + ky * ky);
                const f32 omega = std::sqrt(klen * kG);

                row[n] = Vec4(h0x, h0y, omega, 0.0f);
            }
        }
    }

    void FOceanFFT::SetWindDirection(f32 _Rad) {
        if (_Rad == WindAngle) return;
        WindAngle = _Rad;
        if (H0StagingMapped) {
            ComputeH0();
            H0Dirty = true;
        }
    }

    void FOceanFFT::SetWindSpeed(f32 _V) {
        const f32 V = std::max(_V, 0.1f);
        if (V == WindSpeed) return;
        WindSpeed = V;
        if (H0StagingMapped) {
            ComputeH0();
            H0Dirty = true;
        }
    }

    void FOceanFFT::SetAmplitude(f32 _A) {
        if (_A == Amplitude) return;
        Amplitude = _A;
        if (H0StagingMapped) {
            ComputeH0();
            H0Dirty = true;
        }
    }

    // =====================================================================================
    // Setup GPU
    // =====================================================================================

    Microsoft::WRL::ComPtr<ID3D12Resource> FOceanFFT::Create2D(
        ID3D12Device* _Device, DXGI_FORMAT _Format, u32 _Width, u32 _Height, u32 _Mips,
        bool _AllowUAV, D3D12_RESOURCE_STATES _InitialState) {

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = static_cast<UINT16>(_Mips);
        Desc.Format           = _Format;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = _AllowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                          : D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> Res;
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, _InitialState, nullptr, IID_PPV_ARGS(&Res)));
        return Res;
    }

    void FOceanFFT::CreateTextures(ID3D12Device* _Device) {
        // H0 (N+1)^2 RGBA32F (SRV only) + staging UPLOAD persistente.
        H0Tex = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, M, M, 1, false,
                         D3D12_RESOURCE_STATE_COPY_DEST);
        H0State = D3D12_RESOURCE_STATE_COPY_DEST;
        {
            D3D12_RESOURCE_DESC Desc = H0Tex->GetDesc();
            UINT NumRows = 0; UINT64 RowSize = 0; UINT64 TotalSize = 0;
            _Device->GetCopyableFootprints(&Desc, 0, 1, 0,
                                           &H0Footprint, &NumRows, &RowSize, &TotalSize);

            D3D12_RESOURCE_DESC BufDesc{};
            BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            BufDesc.Width            = TotalSize;
            BufDesc.Height           = 1;
            BufDesc.DepthOrArraySize = 1;
            BufDesc.MipLevels        = 1;
            BufDesc.Format           = DXGI_FORMAT_UNKNOWN;
            BufDesc.SampleDesc       = { 1, 0 };
            BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES UploadHeap{}; UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            SMILE_HR(_Device->CreateCommittedResource(
                &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&H0Staging)));
            SMILE_HR(H0Staging->Map(0, nullptr, reinterpret_cast<void**>(&H0StagingMapped)));
        }

        // Espectros + scratch (RG32F, UAV) e mapas (RGBA32F, UAV).
        SpecH    = Create2D(_Device, DXGI_FORMAT_R32G32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SpecD    = Create2D(_Device, DXGI_FORMAT_R32G32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FFTTemp  = Create2D(_Device, DXGI_FORMAT_R32G32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DispTex  = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        OceanTex = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        NormalTex = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N,
                             kNormalMipCount, true,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto& S : NormalMipState) S = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        D3D12_RESOURCE_DESC CBDesc{};
        CBDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        CBDesc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(OceanCB);
        CBDesc.Height           = 1;
        CBDesc.DepthOrArraySize = 1;
        CBDesc.MipLevels        = 1;
        CBDesc.Format           = DXGI_FORMAT_UNKNOWN;
        CBDesc.SampleDesc       = { 1, 0 };
        CBDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES UploadHeap{}; UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        SMILE_HR(_Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &CBDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        SMILE_HR(CB->Map(0, nullptr, reinterpret_cast<void**>(&MappedCBBase)));
    }

    void FOceanFFT::CreateDescriptors(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        H0SRVSlot          = _SRVHeap.Allocate(1);
        SpecSRVPair        = _SRVHeap.Allocate(2);
        FFTTempSRVSlot     = _SRVHeap.Allocate(1);
        SpecUAVPair        = _SRVHeap.Allocate(2);
        FFTTempUAVSlot     = _SRVHeap.Allocate(1);
        DispSRVSlot        = _SRVHeap.Allocate(1);
        DispUAVSlot        = _SRVHeap.Allocate(1);
        GradUAVPair        = _SRVHeap.Allocate(2);
        OceanSRVSlot       = _SRVHeap.Allocate(1);
        NormalChainSRVSlot = _SRVHeap.Allocate(1);
        const u32 NormalMipUAVRest  = _SRVHeap.Allocate(kNormalMipCount - 1);
        const u32 NormalMipSRVBlock = _SRVHeap.Allocate(kNormalMipCount - 1);

        NormalMipUAVSlot[0] = GradUAVPair + 1;
        for (u32 i = 1; i < kNormalMipCount; ++i)
            NormalMipUAVSlot[i] = NormalMipUAVRest + (i - 1);
        for (u32 i = 0; i + 1 < kNormalMipCount; ++i)
            NormalMipSRVSlot[i] = NormalMipSRVBlock + i;

        D3D12_SHADER_RESOURCE_VIEW_DESC SRV{};
        SRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRV.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;

        SRV.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; SRV.Texture2D.MipLevels = 1;
        _SRVHeap.CreateSRV(_Device, H0Tex.Get(), SRV, H0SRVSlot);

        SRV.Format = DXGI_FORMAT_R32G32_FLOAT;
        _SRVHeap.CreateSRV(_Device, SpecH.Get(), SRV, SpecSRVPair);
        _SRVHeap.CreateSRV(_Device, SpecD.Get(), SRV, SpecSRVPair + 1);
        _SRVHeap.CreateSRV(_Device, FFTTemp.Get(), SRV, FFTTempSRVSlot);

        SRV.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        _SRVHeap.CreateSRV(_Device, DispTex.Get(), SRV, DispSRVSlot);
        _SRVHeap.CreateSRV(_Device, OceanTex.Get(), SRV, OceanSRVSlot);

        SRV.Texture2D.MipLevels = kNormalMipCount;
        _SRVHeap.CreateSRV(_Device, NormalTex.Get(), SRV, NormalChainSRVSlot);

        SRV.Texture2D.MipLevels = 1;
        for (u32 i = 0; i + 1 < kNormalMipCount; ++i) {
            SRV.Texture2D.MostDetailedMip = i;
            _SRVHeap.CreateSRV(_Device, NormalTex.Get(), SRV, NormalMipSRVSlot[i]);
        }
        SRV.Texture2D.MostDetailedMip = 0;

        D3D12_UNORDERED_ACCESS_VIEW_DESC UAV{};
        UAV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        UAV.Format = DXGI_FORMAT_R32G32_FLOAT;
        _SRVHeap.CreateUAV(_Device, SpecH.Get(), UAV, SpecUAVPair);
        _SRVHeap.CreateUAV(_Device, SpecD.Get(), UAV, SpecUAVPair + 1);
        _SRVHeap.CreateUAV(_Device, FFTTemp.Get(), UAV, FFTTempUAVSlot);

        UAV.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        _SRVHeap.CreateUAV(_Device, DispTex.Get(), UAV, DispUAVSlot);
        _SRVHeap.CreateUAV(_Device, OceanTex.Get(), UAV, GradUAVPair);

        for (u32 i = 0; i < kNormalMipCount; ++i) {
            UAV.Texture2D.MipSlice = i;
            _SRVHeap.CreateUAV(_Device, NormalTex.Get(), UAV, NormalMipUAVSlot[i]);
        }
        UAV.Texture2D.MipSlice = 0;
    }

    void FOceanFFT::CreatePipelines(ID3D12Device* _Device) {
        UpdateSpectrumPSO.Initialize(_Device, "OceanUpdateSpectrum.cs_6_0.cso", 1, 2);
        FFTPSO.Initialize(_Device, "OceanFFT.cs_6_0.cso", 1, 1);
        CreateDispPSO.Initialize(_Device, "OceanCreateDisplacement.cs_6_0.cso", 2, 1);
        GradientsPSO.Initialize(_Device, "OceanGradients.cs_6_0.cso", 1, 2);
        NormalMipPSO.Initialize(_Device, "OceanNormalMip.cs_6_0.cso", 1, 1);
    }

    void FOceanFFT::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        CreateTextures(_Device);
        CreateDescriptors(_Device, _SRVHeap);
        CreatePipelines(_Device);
        ComputeH0();
        H0Dirty = true;
        LogInfo("Oceano FFT na GPU: 256^2, espectro com direcao de vento + foam");
    }

    // =====================================================================================
    // Per-frame: pipeline de compute
    // =====================================================================================

    void FOceanFFT::TransitionTex(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _R,
                                  D3D12_RESOURCE_STATES& _State, D3D12_RESOURCE_STATES _Target,
                                  u32 _Subresource) {
        if (_State == _Target) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = _R;
        B.Transition.StateBefore = _State;
        B.Transition.StateAfter  = _Target;
        B.Transition.Subresource = _Subresource;
        _CL->ResourceBarrier(1, &B);
        _State = _Target;
    }

    void FOceanFFT::RecordCompute(u32 _FrameSlot, ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!IsInitialized() || !MappedCBBase) return;

        FrameSlot = _FrameSlot;
        MappedCB = reinterpret_cast<OceanCB*>(
            MappedCBBase + static_cast<size_t>(FrameSlot) * sizeof(OceanCB));

        const auto UAV  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const auto NPS  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto READ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (H0Dirty) {
            TransitionTex(_CL, H0Tex.Get(), H0State, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION Src{};
            Src.pResource       = H0Staging.Get();
            Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            Src.PlacedFootprint = H0Footprint;
            D3D12_TEXTURE_COPY_LOCATION Dst{};
            Dst.pResource        = H0Tex.Get();
            Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            Dst.SubresourceIndex = 0;
            _CL->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
            TransitionTex(_CL, H0Tex.Get(), H0State, NPS);
            H0Dirty = false;
        }

        MappedCB->Time          = SimTime;
        MappedCB->ChoppyScale   = ChoppyWaveScale;
        MappedCB->HeightScale   = MaxWaveSize;
        MappedCB->NormalUp      = NormalUp;
        MappedCB->JacobianScale = ChoppyJacobianScale;
        const D3D12_GPU_VIRTUAL_ADDRESS CBAddr = CB->GetGPUVirtualAddress() +
            static_cast<UINT64>(FrameSlot) * sizeof(OceanCB);

        TransitionTex(_CL, SpecH.Get(), SpecHState, UAV);
        TransitionTex(_CL, SpecD.Get(), SpecDState, UAV);
        UpdateSpectrumPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr);
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(H0SRVSlot));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(SpecUAVPair));
        _CL->Dispatch(N / 16, N / 16, 1);

        auto FFTPass = [&](ID3D12Resource* SrcRes, D3D12_RESOURCE_STATES& SrcState, u32 SrcSRV,
                           ID3D12Resource* DstRes, D3D12_RESOURCE_STATES& DstState, u32 DstUAV) {
            TransitionTex(_CL, SrcRes, SrcState, NPS);
            TransitionTex(_CL, DstRes, DstState, UAV);
            FFTPSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr);
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SrcSRV));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DstUAV));
            _CL->Dispatch(N, 1, 1);
        };
        FFTPass(SpecH.Get(),   SpecHState,   SpecSRVPair,
                FFTTemp.Get(), FFTTempState, FFTTempUAVSlot);
        FFTPass(FFTTemp.Get(), FFTTempState, FFTTempSRVSlot,
                SpecH.Get(),   SpecHState,   SpecUAVPair);
        FFTPass(SpecD.Get(),   SpecDState,   SpecSRVPair + 1,
                FFTTemp.Get(), FFTTempState, FFTTempUAVSlot);
        FFTPass(FFTTemp.Get(), FFTTempState, FFTTempSRVSlot,
                SpecD.Get(),   SpecDState,   SpecUAVPair + 1);

        TransitionTex(_CL, SpecH.Get(), SpecHState, NPS);
        TransitionTex(_CL, SpecD.Get(), SpecDState, NPS);
        TransitionTex(_CL, DispTex.Get(), DispState, UAV);
        CreateDispPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr);
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(SpecSRVPair));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DispUAVSlot));
        _CL->Dispatch(N / 16, N / 16, 1);

        TransitionTex(_CL, DispTex.Get(), DispState, NPS);
        TransitionTex(_CL, OceanTex.Get(), OceanState, UAV);
        TransitionTex(_CL, NormalTex.Get(), NormalMipState[0], UAV, 0);
        GradientsPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr);
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(DispSRVSlot));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(GradUAVPair));
        _CL->Dispatch(N / 16, N / 16, 1);

        NormalMipPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr);
        for (u32 Mip = 1; Mip < kNormalMipCount; ++Mip) {
            TransitionTex(_CL, NormalTex.Get(), NormalMipState[Mip - 1], NPS, Mip - 1);
            TransitionTex(_CL, NormalTex.Get(), NormalMipState[Mip], UAV, Mip);
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(NormalMipSRVSlot[Mip - 1]));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(NormalMipUAVSlot[Mip]));
            const u32 Res    = kGridSize >> Mip;
            const u32 Groups = (Res + 7) / 8;
            _CL->Dispatch(Groups, Groups, 1);
        }

        TransitionTex(_CL, OceanTex.Get(), OceanState, READ);
        for (u32 Mip = 0; Mip < kNormalMipCount; ++Mip)
            TransitionTex(_CL, NormalTex.Get(), NormalMipState[Mip], READ, Mip);
    }
}
