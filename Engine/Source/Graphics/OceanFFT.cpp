#include "Smile/Graphics/OceanFFT.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/OceanSpectrum.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace Smile {
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
        } while (w <= 0.0f || w >= 1.0f);
        w = std::sqrt((-2.0f * std::log(w)) / w);
        GaussianLast    = x2 * w;
        GaussianHasLast = true;
        return x1 * w;
    }

    void FOceanFFT::ComputeH0(u32 _StagingSlot) {
        if (_StagingSlot >= FCommandQueue::kFramesInFlight ||
            !H0StagingMapped[_StagingSlot]) return;

        u8* StagingMapped = H0StagingMapped[_StagingSlot];

        constexpr f32 kTwoPi = 6.28318530717958647692f;
        const f32 DeltaK = kTwoPi / std::max(WorldSize, 1.0e-3f);
        const UINT RowPitch = H0Footprint.Footprint.RowPitch;

        FOceanSpectrumParameters SpectrumParams{};
        SpectrumParams.WindSpeed = WindSpeed;
        SpectrumParams.WindDirection = WindAngle;
        SpectrumParams.FetchKilometres = FetchKilometres;
        SpectrumParams.DepthMetres = OceanDepth;
        SpectrumParams.Swell = Swell;
        // Amplitude is linear at the UI boundary, while energy is quadratic.
        SpectrumParams.Gain = Amplitude * Amplitude;
        const FOceanSpectrum Spectrum(SpectrumParams);

        Rng.seed(Seed);
        GaussianHasLast = false;
        std::uniform_real_distribution<f32> PhaseDistribution(0.0f, kTwoPi);

        for (int Y = 0; Y < N; ++Y) {
            Vec4* Row = reinterpret_cast<Vec4*>(
                StagingMapped + H0Footprint.Offset + static_cast<UINT64>(Y) * RowPitch);
            const int IY = N / 2 - Y;

            for (int X = 0; X < N; ++X) {
                const int IX = N / 2 - X;
                const f32 Kx = static_cast<f32>(IX) * DeltaK;
                const f32 Ky = static_cast<f32>(IY) * DeltaK;
                const f32 Cycles = std::sqrt(static_cast<f32>(IX * IX + IY * IY));

                // Index zero is the self-conjugate Nyquist axis in this centered
                // N-point layout. Its row and column, plus centered DC, are zero.
                const bool IsNyquistAxis = X == 0 || Y == 0;
                const bool IsDC = IX == 0 && IY == 0;
                if (IsNyquistAxis || IsDC ||
                    Cycles < CutoffLowCycles || Cycles >= CutoffHighCycles) {
                    Row[X] = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
                    continue;
                }

                FOceanDispersionSample Dispersion{};
                const f32 Density = Spectrum.WaveVectorDensity(Kx, Ky, &Dispersion);
                // Horvath eq. 47, converted from cosine amplitude to the complex
                // Tessendorf coefficient: one Gaussian amplitude plus uniform phase.
                // E[|h0(k)|^2] = P(k) * DeltaK^2 / 2.
                const f32 CellEnergy = Density * DeltaK * DeltaK;
                const f32 AmplitudeScale = CellEnergy > 0.0f
                    ? std::sqrt(0.5f * CellEnergy) : 0.0f;
                const f32 A = FrandGaussian() * AmplitudeScale;
                const f32 Phase = PhaseDistribution(Rng);
                Row[X] = Vec4(A * std::cos(Phase),
                             -A * std::sin(Phase),
                              Dispersion.Omega, 0.0f);
            }
        }
    }

    void FOceanFFT::ConfigureCascade(u32 _SeedValue, f32 _WorldSizeMeters,
                                     f32 _CyclesLow, f32 _CyclesHigh) {
        const f32 NewWorldSize = std::isfinite(_WorldSizeMeters)
            ? std::max(_WorldSizeMeters, 1.0e-3f) : 64.0f;
        const f32 NewLow = std::isfinite(_CyclesLow) ? std::max(_CyclesLow, 0.0f) : 0.0f;
        const f32 NewHigh = std::isfinite(_CyclesHigh)
            ? std::max(_CyclesHigh, NewLow) : NewLow;
        if (_SeedValue == Seed && NewWorldSize == WorldSize &&
            NewLow == CutoffLowCycles && NewHigh == CutoffHighCycles) return;

        Seed = _SeedValue;
        WorldSize = NewWorldSize;
        CutoffLowCycles = NewLow;
        CutoffHighCycles = NewHigh;
        H0Dirty = true;
        ResetTemporalHistory();
    }

    void FOceanFFT::SetWindDirection(f32 _Rad) {
        const f32 Rad = std::isfinite(_Rad) ? _Rad : 0.0f;
        if (Rad == WindAngle) return;
        WindAngle = Rad;
        H0Dirty = true;
        ResetTemporalHistory();
    }

    void FOceanFFT::SetWindSpeed(f32 _V) {
        const f32 V = std::isfinite(_V) ? std::max(_V, 0.1f) : 4.0f;
        if (V == WindSpeed) return;
        WindSpeed = V;
        H0Dirty = true;
        ResetTemporalHistory();
    }

    void FOceanFFT::SetAmplitude(f32 _A) {
        const f32 A = std::isfinite(_A) ? std::max(_A, 0.0f) : 1.0f;
        if (A == Amplitude) return;
        Amplitude = A;
        H0Dirty = true;
        ResetTemporalHistory();
    }

    void FOceanFFT::SetSpectrumFetch(f32 _Kilometres) {
        const f32 Fetch = std::isfinite(_Kilometres)
            ? std::max(_Kilometres, 0.001f) : 100.0f;
        if (Fetch == FetchKilometres) return;
        FetchKilometres = Fetch;
        H0Dirty = true;
        ResetTemporalHistory();
    }

    void FOceanFFT::SetOceanDepth(f32 _Metres) {
        const f32 Depth = std::isfinite(_Metres) ? std::max(_Metres, 0.01f) : 100.0f;
        if (Depth == OceanDepth) return;
        OceanDepth = Depth;
        H0Dirty = true;
        ResetTemporalHistory();
    }

    void FOceanFFT::SetSwell(f32 _Value) {
        const f32 Value = std::isfinite(_Value) ? std::clamp(_Value, 0.0f, 1.0f) : 0.25f;
        if (Value == Swell) return;
        Swell = Value;
        H0Dirty = true;
        ResetTemporalHistory();
    }

    void FOceanFFT::SetGeometryScales(f32 _HeightScale, f32 _ChoppyLambda) {
        SurfaceHeightScale = std::isfinite(_HeightScale)
            ? std::max(_HeightScale, 0.0f) : 1.0f;
        ChoppyLambda = std::isfinite(_ChoppyLambda)
            ? std::max(_ChoppyLambda, 0.0f) : 1.0f;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> FOceanFFT::Create2D(
        ID3D12Device* _Device, DXGI_FORMAT _Format, u32 _Width, u32 _Height, u32 _Mips,
        bool _AllowUAV, D3D12_RESOURCE_STATES _InitialState) {

        return GpuResources::CreateTex2D(
            _Device, _Width, _Height, _Format,
            _AllowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                      : D3D12_RESOURCE_FLAG_NONE,
            _InitialState, EVramCategory::Water, nullptr, _Mips, 1, "FFT do oceano");
    }

    void FOceanFFT::CreateTextures(ID3D12Device* _Device) {
        H0Tex = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, 1, false,
                         D3D12_RESOURCE_STATE_COPY_DEST);
        H0State = D3D12_RESOURCE_STATE_COPY_DEST;
        {
            D3D12_RESOURCE_DESC Desc = H0Tex->GetDesc();
            UINT NumRows = 0; UINT64 RowSize = 0; UINT64 TotalSize = 0;
            _Device->GetCopyableFootprints(&Desc, 0, 1, 0,
                                           &H0Footprint, &NumRows, &RowSize, &TotalSize);

            // Um staging POR SLOT (nao um sliced): cada um e escrito pela CPU e copiado numa
            // fence propria, entao eles precisam de recursos distintos.
            for (u32 Slot = 0; Slot < FCommandQueue::kFramesInFlight; ++Slot) {
                const GpuResources::FUploadBuffer Upload =
                    GpuResources::CreateUploadBuffer(_Device, TotalSize, 1, false);
                H0Staging[Slot]       = Upload.Resource;
                H0StagingMapped[Slot] = Upload.Mapped;
            }
        }

        SpecH    = Create2D(_Device, DXGI_FORMAT_R32G32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SpecD    = Create2D(_Device, DXGI_FORMAT_R32G32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        FFTTemp  = Create2D(_Device, DXGI_FORMAT_R32G32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DispTex  = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, 1, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        OceanTex = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N,
                            kDisplacementMipCount, true,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        PreviousOceanTex = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N,
                                    kDisplacementMipCount, false,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        for (auto& S : OceanMipState) S = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        for (auto& S : PreviousOceanMipState) S = D3D12_RESOURCE_STATE_COPY_DEST;
        DisplacementHistoryValid = false;
        NormalTex = Create2D(_Device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N,
                             kNormalMipCount, true,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto& S : NormalMipState) S = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        static_assert(sizeof(OceanCB) % 256 == 0,
                      "o CB e indexado por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(OceanCB), FCommandQueue::kFramesInFlight);
        CB            = Upload.Resource;
        MappedCBBase  = Upload.Mapped;
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
        PreviousOceanSRVSlot = _SRVHeap.Allocate(1);
        const u32 OceanMipUAVRest = _SRVHeap.Allocate(kDisplacementMipCount - 1);
        const u32 OceanMipSRVBlock = _SRVHeap.Allocate(kDisplacementMipCount - 1);
        NormalChainSRVSlot = _SRVHeap.Allocate(1);
        const u32 NormalMipUAVRest  = _SRVHeap.Allocate(kNormalMipCount - 1);
        const u32 NormalMipSRVBlock = _SRVHeap.Allocate(kNormalMipCount - 1);

        NormalMipUAVSlot[0] = GradUAVPair + 1;
        OceanMipUAVSlot[0] = GradUAVPair;
        for (u32 i = 1; i < kDisplacementMipCount; ++i)
            OceanMipUAVSlot[i] = OceanMipUAVRest + (i - 1);
        for (u32 i = 0; i + 1 < kDisplacementMipCount; ++i)
            OceanMipSRVSlot[i] = OceanMipSRVBlock + i;
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
        SRV.Texture2D.MipLevels = kDisplacementMipCount;
        _SRVHeap.CreateSRV(_Device, OceanTex.Get(), SRV, OceanSRVSlot);
        _SRVHeap.CreateSRV(_Device, PreviousOceanTex.Get(), SRV, PreviousOceanSRVSlot);

        SRV.Texture2D.MipLevels = 1;
        for (u32 i = 0; i + 1 < kDisplacementMipCount; ++i) {
            SRV.Texture2D.MostDetailedMip = i;
            _SRVHeap.CreateSRV(_Device, OceanTex.Get(), SRV, OceanMipSRVSlot[i]);
        }
        SRV.Texture2D.MostDetailedMip = 0;

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
        for (u32 i = 0; i < kDisplacementMipCount; ++i) {
            UAV.Texture2D.MipSlice = i;
            _SRVHeap.CreateUAV(_Device, OceanTex.Get(), UAV, OceanMipUAVSlot[i]);
        }

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
        DisplacementMipPSO.Initialize(_Device, "OceanDisplacementMip.cs_6_0.cso", 1, 1);
        NormalMipPSO.Initialize(_Device, "OceanNormalMip.cs_6_0.cso", 1, 1);
    }

    void FOceanFFT::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        CreateTextures(_Device);
        CreateDescriptors(_Device, _SRVHeap);
        CreatePipelines(_Device);
        H0Dirty = true;
    }

    void FOceanFFT::RecreatePipelines(ID3D12Device* _Device) {
        CreatePipelines(_Device);
    }

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

        // Conserva o deslocamento do frame anterior antes de sobrescrever OceanTex. A fila e
        // serial, portanto uma unica textura de historico por cascata basta mesmo com dois frames
        // de CB em voo: o copy fica ordenado depois do draw anterior e antes do compute atual.
        const bool HadDisplacementHistory = DisplacementHistoryValid;
        if (HadDisplacementHistory) {
            for (u32 Mip = 0; Mip < kDisplacementMipCount; ++Mip) {
                TransitionTex(_CL, OceanTex.Get(), OceanMipState[Mip],
                              D3D12_RESOURCE_STATE_COPY_SOURCE, Mip);
                TransitionTex(_CL, PreviousOceanTex.Get(), PreviousOceanMipState[Mip],
                              D3D12_RESOURCE_STATE_COPY_DEST, Mip);
            }
            _CL->CopyResource(PreviousOceanTex.Get(), OceanTex.Get());
            for (u32 Mip = 0; Mip < kDisplacementMipCount; ++Mip)
                TransitionTex(_CL, PreviousOceanTex.Get(), PreviousOceanMipState[Mip], READ, Mip);
        }

        if (H0Dirty) {
            if (!H0Staging[FrameSlot] || !H0StagingMapped[FrameSlot]) return;
            ComputeH0(FrameSlot);
            TransitionTex(_CL, H0Tex.Get(), H0State, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION Src{};
            Src.pResource       = H0Staging[FrameSlot].Get();
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

        const f32 Dt = std::clamp(RealTime - LastRealTime, 0.0f, 0.1f);
        LastRealTime = RealTime;

        MappedCB->Time             = SimTime;
        MappedCB->ChoppyScale      = SurfaceHeightScale * ChoppyLambda;
        MappedCB->HeightScale      = SurfaceHeightScale;
        MappedCB->InvTwoTexelWorld = 0.5f * static_cast<f32>(N) /
                                      std::max(WorldSize, 1.0e-3f);
        MappedCB->DeltaTime        = Dt;
        MappedCB->FoamRecovery     = FoamRecovery;
        MappedCB->FoamReset        = FoamHistoryValid ? 0.0f : 1.0f;
        MappedCB->Padding0         = 0.0f;
        MappedCB->WindDirectionX   = std::cos(WindAngle);
        MappedCB->WindDirectionZ   = std::sin(WindAngle);
        MappedCB->Padding1         = 0.0f;
        MappedCB->Padding2         = 0.0f;
        FoamHistoryValid = true;
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
        TransitionTex(_CL, OceanTex.Get(), OceanMipState[0], UAV, 0);
        TransitionTex(_CL, NormalTex.Get(), NormalMipState[0], UAV, 0);
        GradientsPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr);
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(DispSRVSlot));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(GradUAVPair));
        _CL->Dispatch(N / 16, N / 16, 1);

        // Low-pass displacement/J chain used by the vertex shader. This filters
        // frequencies inside each broad cascade instead of fading the whole band
        // when its shortest waves become sub-pixel.
        DisplacementMipPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr);
        for (u32 Mip = 1; Mip < kDisplacementMipCount; ++Mip) {
            TransitionTex(_CL, OceanTex.Get(), OceanMipState[Mip - 1], NPS, Mip - 1);
            TransitionTex(_CL, OceanTex.Get(), OceanMipState[Mip], UAV, Mip);
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(OceanMipSRVSlot[Mip - 1]));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(OceanMipUAVSlot[Mip]));
            const u32 Res = kGridSize >> Mip;
            _CL->Dispatch((Res + 7) / 8, (Res + 7) / 8, 1);
        }

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

        for (u32 Mip = 0; Mip < kDisplacementMipCount; ++Mip)
            TransitionTex(_CL, OceanTex.Get(), OceanMipState[Mip], READ, Mip);
        for (u32 Mip = 0; Mip < kNormalMipCount; ++Mip)
            TransitionTex(_CL, NormalTex.Get(), NormalMipState[Mip], READ, Mip);

        // Primeiro frame (ou reset temporal): inicializa prev=current para produzir velocity zero
        // em vez de ler VRAM indefinida. Nos frames seguintes o copy do topo preserva a fase N-1.
        if (!HadDisplacementHistory) {
            for (u32 Mip = 0; Mip < kDisplacementMipCount; ++Mip) {
                TransitionTex(_CL, OceanTex.Get(), OceanMipState[Mip],
                              D3D12_RESOURCE_STATE_COPY_SOURCE, Mip);
                TransitionTex(_CL, PreviousOceanTex.Get(), PreviousOceanMipState[Mip],
                              D3D12_RESOURCE_STATE_COPY_DEST, Mip);
            }
            _CL->CopyResource(PreviousOceanTex.Get(), OceanTex.Get());
            for (u32 Mip = 0; Mip < kDisplacementMipCount; ++Mip) {
                TransitionTex(_CL, OceanTex.Get(), OceanMipState[Mip], READ, Mip);
                TransitionTex(_CL, PreviousOceanTex.Get(), PreviousOceanMipState[Mip], READ, Mip);
            }
            DisplacementHistoryValid = true;
        }
    }

    FPassShaderStems FOceanFFT::ShaderStems() const {
        static const char* const kStems[] = { "OceanUpdateSpectrum.cs", "OceanFFT.cs", "OceanCreateDisplacement.cs", "OceanGradients.cs", "OceanDisplacementMip.cs", "OceanNormalMip.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FOceanFFT::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (IsInitialized()) RecreatePipelines(_Ctx.Device);
    }

}
