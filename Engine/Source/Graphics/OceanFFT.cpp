#include "Smile/Graphics/OceanFFT.h"
#include "Smile/Core/HResultCheck.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Smile {

    // =====================================================================================
    // Simulacao — porte verbatim do CWaterSim (WaterUtils.cpp)
    // =====================================================================================

    // Box-Muller (WaterUtils.cpp:142). cry_random(-1,1) -> uniform real em [-1,1].
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

    // Phillips (WaterUtils.cpp:362). pW = -(cos(wind), sin(wind)) (InitFourierAmps:406-407);
    // com wind=0 -> pW=(-1,0), |w|^2=1, L=1/g.
    f32 FOceanFFT::ComputePhillips(f32 kx, f32 ky) const {
        const f32 k2 = kx * kx + ky * ky;
        if (k2 == 0.0f) return 0.0f;

        const f32 wx = -std::cos(WindAngle);
        const f32 wy = -std::sin(WindAngle);
        const f32 w2 = wx * wx + wy * wy;       // 1
        const f32 L  = w2 / kG;
        const f32 L2 = L * L;
        const f32 kDotW = kx * wx + ky * wy;

        return Amplitude * (std::exp(-1.0f / (k2 * L2)) / (k2 * k2)) * (kDotW * kDotW / k2 * w2);
    }

    // LUT de k (WaterUtils.cpp:386). GetIndexToWorld = (i - N/2) * 2*PI/worldSize.
    void FOceanFFT::InitTableK() {
        const f32 pi2OverWorld = (2.0f * 3.14159265358979323846f) / WorldSize;
        for (int y = 0; y < N; ++y) {
            const f32 ky = (f32(y) - (N / 2.0f)) * pi2OverWorld;
            for (int x = 0; x < N; ++x) {
                const f32 kx   = (f32(x) - (N / 2.0f)) * pi2OverWorld;
                const f32 klen = std::sqrt(kx * kx + ky * ky);
                const f32 omega = std::sqrt(klen * kG); // GetTermAngularFreq
                LUTK[Offset(x, y)] = Vec4(kx, ky, klen, omega);
            }
        }
    }

    // H0 (WaterUtils.cpp:402). e = (gaussian, gaussian) * (1/sqrt2) * sqrt(Phillips).
    void FOceanFFT::InitFourierAmps() {
        const f32 recipSqrt2 = 1.0f / std::sqrt(2.0f);
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const int off = Offset(x, y);
                complexF e(FrandGaussian(), FrandGaussian());
                const Vec4 k = LUTK[off];
                e *= recipSqrt2 * std::sqrt(ComputePhillips(k.X, k.Y));
                FourierAmps[off] = e;
            }
        }
    }

    // FFT 1D Cooley-Tukey (WaterUtils.cpp:198, ref. Paul Bourke). Dir=-1 = inversa, SEM
    // normalizar (so Dir=+1 divide por N) — quirk fiel da Cry.
    void FOceanFFT::ComputeFFT1D(int Dir, f32* pReal, f32* pImag) {
        const long nn = N;
        const f32 fRecipNN = 1.0f / f32(nn);

        // Bit reversal.
        long i2 = nn >> 1;
        long j = 0;
        for (long i = 0; i < nn - 1; ++i) {
            if (i < j) {
                f32 tr = pReal[i]; f32 ti = pImag[i];
                pReal[i] = pReal[j]; pImag[i] = pImag[j];
                pReal[j] = tr;       pImag[j] = ti;
            }
            long k = i2;
            while (k <= j) { j -= k; k >>= 1; }
            j += k;
        }

        // Butterflies.
        f32 c1 = -1.0f, c2 = 0.0f;
        long l2 = 1;
        for (long l = 0; l < (long)kLogGridSize; ++l) {
            long l1 = l2;
            l2 <<= 1;
            f32 u1 = 1.0f, u2 = 0.0f;
            for (j = 0; j < l1; ++j) {
                for (long i = j; i < nn; i += l2) {
                    long i1 = i + l1;
                    f32 t1 = u1 * pReal[i1] - u2 * pImag[i1];
                    f32 t2 = u1 * pImag[i1] + u2 * pReal[i1];
                    pReal[i1] = pReal[i] - t1;
                    pImag[i1] = pImag[i] - t2;
                    pReal[i] += t1;
                    pImag[i] += t2;
                }
                f32 z = u1 * c1 - u2 * c2;
                u2 = u1 * c2 + u2 * c1;
                u1 = z;
            }
            c2 = std::sqrt((1.0f - c1) * 0.5f);
            if (Dir == 1) c2 = -c2;
            c1 = std::sqrt((1.0f + c1) * 0.5f);
        }

        // Normalizacao so no forward.
        if (Dir == 1) {
            for (long i = 0; i < nn; ++i) { pReal[i] *= fRecipNN; pImag[i] *= fRecipNN; }
        }
    }

    // FFT 2D: linhas depois colunas (WaterUtils.cpp:284).
    void FOceanFFT::ComputeFFT2D(int Dir, complexF* c) {
        f32 pReal[N];
        f32 pImag[N];

        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const complexF& cur = c[Offset(x, y)];
                pReal[x] = cur.real(); pImag[x] = cur.imag();
            }
            ComputeFFT1D(Dir, pReal, pImag);
            for (int x = 0; x < N; ++x) c[Offset(x, y)] = complexF(pReal[x], pImag[x]);
        }
        for (int x = 0; x < N; ++x) {
            for (int y = 0; y < N; ++y) {
                const complexF& cur = c[Offset(x, y)];
                pReal[y] = cur.real(); pImag[y] = cur.imag();
            }
            ComputeFFT1D(Dir, pReal, pImag);
            for (int y = 0; y < N; ++y) c[Offset(x, y)] = complexF(pReal[y], pImag[y]);
        }
    }

    // Evolucao temporal + IFFT + empacotamento (WaterUtils.cpp:435).
    void FOceanFFT::UpdateSim(f32 fTime) {
        const int halfY = (N >> 1) + 1; // 33
        for (int y = 0; y < halfY; ++y) {
            for (int x = 0; x < N; ++x) {
                const int off    = Offset(x, y);
                const int offMir = OffsetMirror(x, y);

                const Vec4 k = LUTK[off];
                const f32 klen = k.Z;
                const f32 angFreq = k.W * fTime;
                const f32 s = std::sin(angFreq);
                const f32 cse = std::cos(angFreq);

                const complexF ep(cse, s);
                const complexF em = std::conj(ep);

                // H(k,t) = H0(k)*e^{iwt} + conj(H0(-k))*e^{-iwt}
                const complexF wave = FourierAmps[off] * ep + std::conj(FourierAmps[offMir]) * em;

                HeightField[off] = wave;
                if (klen != 0.0f) {
                    DisplaceX[off] = wave * complexF(0.0f, (-k.X - k.Y) / klen);
                    DisplaceY[off] = wave * complexF(0.0f, (-k.Y) / klen);
                } else {
                    DisplaceX[off] = complexF(0.0f);
                    DisplaceY[off] = complexF(0.0f);
                }

                if (y != halfY - 1) {
                    HeightField[offMir] = std::conj(HeightField[off]);
                    DisplaceX[offMir]   = std::conj(DisplaceX[off]);
                    DisplaceY[offMir]   = std::conj(DisplaceY[off]);
                }
            }
        }

        ComputeFFT2D(-1, HeightField);
        ComputeFFT2D(-1, DisplaceX);
        ComputeFFT2D(-1, DisplaceY);

        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const int off = Offset(x, y);
                const f32 sign = (f32)PowNeg1(x + y);

                HeightField[off] *= sign * MaxWaveSize;
                DisplaceX[off]   *= sign * ChoppyWaveScale;
                DisplaceY[off]   *= sign * ChoppyWaveScale;

                // (Dx, Dy, -h, 0) — convencao Cry (Z = -altura).
                DisplaceGrid[off] = Vec4(DisplaceX[off].real(),
                                         DisplaceY[off].real(),
                                         -HeightField[off].real(),
                                         0.0f);
            }
        }
    }

    u32 FOceanFFT::NormalMipSize(u32 _Mip) {
        return std::max(1u, kGridSize >> _Mip);
    }

    u32 FOceanFFT::NormalMipOffset(u32 _Mip) {
        u32 Offset = 0;
        for (u32 Mip = 0; Mip < _Mip; ++Mip) {
            const u32 Size = NormalMipSize(Mip);
            Offset += Size * Size;
        }
        return Offset;
    }

    void FOceanFFT::BuildNormalMipChain() {
        constexpr f32 kNormalUp = 8.0f; // WaterVolumesNormalGenPS usa float3(dx,dy,8).

        // Mip 0: normal de mundo Y-up derivada da altura FFT.
        Vec4* Mip0 = NormalMipChain;
        for (u32 y = 0; y < kGridSize; ++y) {
            for (u32 x = 0; x < kGridSize; ++x) {
                const u32 x1 = (x + 1u) & (kGridSize - 1u);
                const u32 y1 = (y + 1u) & (kGridSize - 1u);

                const f32 hC = DisplaceGrid[Offset((int)x,  (int)y )].Z;
                const f32 hX = DisplaceGrid[Offset((int)x1, (int)y )].Z;
                const f32 hZ = DisplaceGrid[Offset((int)x,  (int)y1)].Z;

                f32 nx = hC - hX;
                f32 ny = kNormalUp;
                f32 nz = hC - hZ;
                const f32 Len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (Len > 1e-6f) {
                    const f32 InvLen = 1.0f / Len;
                    nx *= InvLen; ny *= InvLen; nz *= InvLen;
                } else {
                    nx = 0.0f; ny = 1.0f; nz = 0.0f;
                }

                Mip0[y * kGridSize + x] = Vec4(nx, ny, nz, 1.0f);
            }
        }

        // Mips seguintes: media vetorial + Toksvig T propagado no alpha.
        for (u32 Mip = 1; Mip < kNormalMipCount; ++Mip) {
            const u32 SrcSize = NormalMipSize(Mip - 1);
            const u32 DstSize = NormalMipSize(Mip);
            const Vec4* Src = NormalMipChain + NormalMipOffset(Mip - 1);
            Vec4* Dst = NormalMipChain + NormalMipOffset(Mip);

            for (u32 y = 0; y < DstSize; ++y) {
                for (u32 x = 0; x < DstSize; ++x) {
                    f32 sx = 0.0f, sy = 0.0f, sz = 0.0f;
                    for (u32 oy = 0; oy < 2; ++oy) {
                        for (u32 ox = 0; ox < 2; ++ox) {
                            const u32 SrcX = std::min(x * 2u + ox, SrcSize - 1u);
                            const u32 SrcY = std::min(y * 2u + oy, SrcSize - 1u);
                            const Vec4 N = Src[SrcY * SrcSize + SrcX];
                            const f32 T = std::clamp(N.W, 0.0f, 1.0f);
                            sx += N.X * T;
                            sy += N.Y * T;
                            sz += N.Z * T;
                        }
                    }

                    const f32 SumLen = std::sqrt(sx * sx + sy * sy + sz * sz);
                    f32 T = std::clamp(SumLen * 0.25f, 1e-4f, 1.0f);
                    f32 nx = 0.0f, ny = 1.0f, nz = 0.0f;
                    if (SumLen > 1e-6f) {
                        const f32 InvLen = 1.0f / SumLen;
                        nx = sx * InvLen;
                        ny = sy * InvLen;
                        nz = sz * InvLen;
                    }
                    Dst[y * DstSize + x] = Vec4(nx, ny, nz, T);
                }
            }
        }
    }

    void FOceanFFT::StageDisplacement() {
        if (!StagingMapped) return;
        const u32 SrcRowBytes = kGridSize * 4u * sizeof(f32);
        for (u32 row = 0; row < kGridSize; ++row) {
            memcpy(StagingMapped + Footprint.Offset + (UINT64)row * Footprint.Footprint.RowPitch,
                   reinterpret_cast<const u8*>(DisplaceGrid) + (UINT64)row * SrcRowBytes,
                   SrcRowBytes);
        }
    }

    void FOceanFFT::StageNormalMipChain() {
        if (!NormalStagingMapped) return;
        for (u32 Mip = 0; Mip < kNormalMipCount; ++Mip) {
            const u32 Size = NormalMipSize(Mip);
            const u32 SrcRowBytes = Size * 4u * sizeof(f32);
            const Vec4* SrcMip = NormalMipChain + NormalMipOffset(Mip);
            for (u32 row = 0; row < Size; ++row) {
                memcpy(NormalStagingMapped + NormalFootprints[Mip].Offset +
                           (UINT64)row * NormalFootprints[Mip].Footprint.RowPitch,
                       reinterpret_cast<const u8*>(SrcMip) + (UINT64)row * SrcRowBytes,
                       SrcRowBytes);
            }
        }
    }

    // =====================================================================================
    // GPU
    // =====================================================================================

    void FOceanFFT::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        // Espectro inicial (LUT + H0).
        InitTableK();
        InitFourierAmps();

        // Textura DEFAULT 64x64 RGBA32F (s_ptexWaterOcean da Cry).
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = kGridSize;
        Desc.Height           = kGridSize;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES DefaultHeap{}; DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&OceanTex)));
        CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;

        // Footprint + staging UPLOAD persistente (mapeado).
        UINT NumRows = 0; UINT64 RowSize = 0; UINT64 TotalSize = 0;
        _Device->GetCopyableFootprints(&Desc, 0, 1, 0, &Footprint, &NumRows, &RowSize, &TotalSize);

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
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StagingBuf)));
        SMILE_HR(StagingBuf->Map(0, nullptr, reinterpret_cast<void**>(&StagingMapped)));

        // Textura normal derivada do FFT, com mip chain para o PS (WaterNormalTex).
        D3D12_RESOURCE_DESC NormalDesc = Desc;
        NormalDesc.MipLevels = kNormalMipCount;
        SMILE_HR(_Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &NormalDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&NormalTex)));
        NormalCurrentState = D3D12_RESOURCE_STATE_COPY_DEST;

        _Device->GetCopyableFootprints(&NormalDesc, 0, kNormalMipCount, 0,
                                       NormalFootprints, NormalNumRows, NormalRowSize,
                                       &NormalTotalSize);

        BufDesc.Width = NormalTotalSize;
        SMILE_HR(_Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&NormalStagingBuf)));
        SMILE_HR(NormalStagingBuf->Map(0, nullptr, reinterpret_cast<void**>(&NormalStagingMapped)));

        // Estado inicial (t=0) -> staging, p/ o 1o RecordUpload ter dados validos.
        UpdateSim(0.0f);
        BuildNormalMipChain();
        StageDisplacement();
        StageNormalMipChain();

        // SRV no heap compartilhado.
        TexSRVSlot = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, OceanTex.Get(), SRVDesc, TexSRVSlot);

        NormalTexSRVSlot = _SRVHeap.Allocate(1);
        SRVDesc.Texture2D.MipLevels     = kNormalMipCount;
        SRVDesc.Texture2D.MostDetailedMip = 0;
        SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        _SRVHeap.CreateSRV(_Device, NormalTex.Get(), SRVDesc, NormalTexSRVSlot);
    }

    void FOceanFFT::Update(f32 _ElapsedTime) {
        if (!StagingMapped) return;
        // tempo de sim = 0.125 * elapsed (CREWaterOcean::FrameUpdate:346).
        UpdateSim(0.125f * _ElapsedTime);
        BuildNormalMipChain();
        StageDisplacement();
        StageNormalMipChain();
    }

    void FOceanFFT::RecordUpload(ID3D12GraphicsCommandList* _CommandList) {
        if (!OceanTex) return;

        const D3D12_RESOURCE_STATES ReadState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (CurrentState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = OceanTex.Get();
            B.Transition.StateBefore = CurrentState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            _CommandList->ResourceBarrier(1, &B);
            CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        if (NormalTex && NormalCurrentState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = NormalTex.Get();
            B.Transition.StateBefore = NormalCurrentState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            _CommandList->ResourceBarrier(1, &B);
            NormalCurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        D3D12_TEXTURE_COPY_LOCATION SrcLoc{};
        SrcLoc.pResource       = StagingBuf.Get();
        SrcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        SrcLoc.PlacedFootprint = Footprint;
        D3D12_TEXTURE_COPY_LOCATION DstLoc{};
        DstLoc.pResource        = OceanTex.Get();
        DstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        DstLoc.SubresourceIndex = 0;
        _CommandList->CopyTextureRegion(&DstLoc, 0, 0, 0, &SrcLoc, nullptr);

        if (NormalTex) {
            for (u32 Mip = 0; Mip < kNormalMipCount; ++Mip) {
                D3D12_TEXTURE_COPY_LOCATION NormalSrc{};
                NormalSrc.pResource       = NormalStagingBuf.Get();
                NormalSrc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                NormalSrc.PlacedFootprint = NormalFootprints[Mip];

                D3D12_TEXTURE_COPY_LOCATION NormalDst{};
                NormalDst.pResource        = NormalTex.Get();
                NormalDst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                NormalDst.SubresourceIndex = Mip;

                _CommandList->CopyTextureRegion(&NormalDst, 0, 0, 0, &NormalSrc, nullptr);
            }
        }

        D3D12_RESOURCE_BARRIER Barriers[2]{};
        UINT BarrierCount = 0;
        Barriers[BarrierCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barriers[BarrierCount].Transition.pResource   = OceanTex.Get();
        Barriers[BarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        Barriers[BarrierCount].Transition.StateAfter  = ReadState;
        Barriers[BarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++BarrierCount;
        if (NormalTex) {
            Barriers[BarrierCount].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barriers[BarrierCount].Transition.pResource   = NormalTex.Get();
            Barriers[BarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            Barriers[BarrierCount].Transition.StateAfter  = ReadState;
            Barriers[BarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++BarrierCount;
        }
        _CommandList->ResourceBarrier(BarrierCount, Barriers);
        CurrentState = ReadState;
        NormalCurrentState = ReadState;
    }
}
