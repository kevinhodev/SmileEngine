#include "Smile/Graphics/OceanWater.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    namespace {
        constexpr f32 kGravity = 9.81f;
        constexpr f32 kSimTimeScale = 0.125f;
        constexpr f32 kHeightOutputScale = 0.006f;
        constexpr f32 kChoppyOutputScale = 0.006f;

        UINT64 AlignUp(UINT64 Value, UINT64 Align) {
            return (Value + Align - 1) & ~(Align - 1);
        }

        std::vector<u8> LoadShader(const std::string& Name) {
            const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + Name;
            std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
            if (!File) {
                LogError("Falha ao abrir shader de agua: " + FullPath);
                throw std::runtime_error("Water shader nao encontrado: " + FullPath);
            }
            const auto Size = static_cast<size_t>(File.tellg());
            std::vector<u8> Data(Size);
            File.seekg(0);
            File.read(reinterpret_cast<char*>(Data.data()), Size);
            return Data;
        }
    }

    class FOceanWater::FOceanFFT {
    public:
        static constexpr i32 GridSize = static_cast<i32>(FOceanWater::kDisplacementSize);
        static constexpr i32 LogGridSize = 6;
        static constexpr i32 CellCount = GridSize * GridSize;

        void Update(f32 TimeSeconds, const FOceanSettings& Settings) {
            const f32 WindSpeed = std::max(Settings.WindSpeed, 0.1f);
            const f32 WaveSize = std::max(Settings.WaveSize, 1.0f);
            if (!Initialized ||
                std::fabs(WindDirectionRad - Settings.WindDirectionRad) > 1e-4f ||
                std::fabs(WindSpeedCached - WindSpeed) > 1e-3f ||
                std::fabs(WaveSizeCached - WaveSize) > 1e-3f) {
                Initialize(Settings.WindDirectionRad, WindSpeed, WaveSize);
            }

            const f32 SimTime = TimeSeconds * kSimTimeScale;
            const i32 HalfY = (GridSize >> 1) + 1;

            for (i32 y = 0; y < HalfY; ++y) {
                for (i32 x = 0; x < GridSize; ++x) {
                    const i32 Offset = GridOffset(x, y);
                    const i32 Mirror = MirroredGridOffset(x, y);

                    const Vec4& K = LUTK[Offset];
                    const f32 KLen = K.Z;
                    const f32 Phase = K.W * SimTime;
                    const f32 S = std::sin(Phase);
                    const f32 C = std::cos(Phase);

                    const Complex Ep(C, S);
                    const Complex Em = std::conj(Ep);
                    const Complex Wave = FourierAmps[Offset] * Ep +
                                         std::conj(FourierAmps[Mirror]) * Em;

                    HeightField[Offset] = Wave;
                    if (KLen > 1e-6f) {
                        DisplaceFieldX[Offset] = Wave * Complex(0.0f, -K.X / KLen);
                        DisplaceFieldZ[Offset] = Wave * Complex(0.0f, -K.Y / KLen);
                    } else {
                        DisplaceFieldX[Offset] = Complex(0.0f, 0.0f);
                        DisplaceFieldZ[Offset] = Complex(0.0f, 0.0f);
                    }

                    if (y != HalfY - 1) {
                        HeightField[Mirror] = std::conj(HeightField[Offset]);
                        DisplaceFieldX[Mirror] = std::conj(DisplaceFieldX[Offset]);
                        DisplaceFieldZ[Mirror] = std::conj(DisplaceFieldZ[Offset]);
                    }
                }
            }

            ComputeFFT2D(-1, HeightField);
            ComputeFFT2D(-1, DisplaceFieldX);
            ComputeFFT2D(-1, DisplaceFieldZ);

            const f32 MaxHeight = std::clamp(Settings.WaveSize * 0.06f, 0.5f, 24.0f);
            const f32 MaxHorizontal = std::clamp(Settings.WaveSize * 0.08f, 0.5f, 24.0f);

            for (i32 y = 0; y < GridSize; ++y) {
                for (i32 x = 0; x < GridSize; ++x) {
                    const i32 Offset = GridOffset(x, y);
                    const f32 Checker = ((x + y) & 1) ? -1.0f : 1.0f;

                    f32 OffsetX = DisplaceFieldX[Offset].real() * Checker * kChoppyOutputScale;
                    f32 OffsetZ = DisplaceFieldZ[Offset].real() * Checker * kChoppyOutputScale;
                    f32 Height = -HeightField[Offset].real() * Checker * kHeightOutputScale;

                    if (!std::isfinite(OffsetX)) OffsetX = 0.0f;
                    if (!std::isfinite(OffsetZ)) OffsetZ = 0.0f;
                    if (!std::isfinite(Height)) Height = 0.0f;

                    OffsetX = std::clamp(OffsetX, -MaxHorizontal, MaxHorizontal);
                    OffsetZ = std::clamp(OffsetZ, -MaxHorizontal, MaxHorizontal);
                    Height = std::clamp(Height, -MaxHeight, MaxHeight);

                    const f32 Crest = std::clamp(
                        std::fabs(Height) / MaxHeight +
                        std::sqrt(OffsetX * OffsetX + OffsetZ * OffsetZ) / MaxHorizontal,
                        0.0f, 1.0f);

                    DisplacementGrid[Offset] = Vec4(OffsetX, Height, OffsetZ, Crest);
                }
            }
        }

        const std::array<Vec4, CellCount>& GetDisplacements() const {
            return DisplacementGrid;
        }

    private:
        using Complex = std::complex<f32>;

        void Initialize(f32 NewWindDirectionRad, f32 NewWindSpeed, f32 NewWaveSize) {
            WindDirectionRad = NewWindDirectionRad;
            WindSpeedCached = NewWindSpeed;
            WaveSizeCached = NewWaveSize;
            InitTableK();
            InitFourierAmps();
            Initialized = true;
        }

        static i32 GridOffset(i32 X, i32 Y) {
            return Y * GridSize + X;
        }

        static i32 MirroredGridOffset(i32 X, i32 Y) {
            return GridOffset((GridSize - X) & (GridSize - 1),
                              (GridSize - Y) & (GridSize - 1));
        }

        static f32 PowNegOne(i32 N) {
            return (N & 1) ? -1.0f : 1.0f;
        }

        f32 IndexToWaveNumber(i32 Index) const {
            const f32 WaveNumberScale = TwoPi / std::max(WaveSizeCached, 1.0f);
            return (static_cast<f32>(Index) - static_cast<f32>(GridSize) * 0.5f) * WaveNumberScale;
        }

        static f32 AngularFrequency(f32 KLen) {
            return std::sqrt(std::max(KLen * kGravity, 0.0f));
        }

        void InitTableK() {
            for (i32 y = 0; y < GridSize; ++y) {
                const f32 Kz = IndexToWaveNumber(y);
                for (i32 x = 0; x < GridSize; ++x) {
                    const f32 Kx = IndexToWaveNumber(x);
                    const f32 KLen = std::sqrt(Kx * Kx + Kz * Kz);
                    LUTK[GridOffset(x, y)] = Vec4(Kx, Kz, KLen, AngularFrequency(KLen));
                }
            }
        }

        f32 PhillipsSpectrum(f32 Kx, f32 Kz) const {
            const f32 K2 = Kx * Kx + Kz * Kz;
            if (K2 < 1e-8f) {
                return 0.0f;
            }

            const f32 WindX = -std::cos(WindDirectionRad);
            const f32 WindZ = -std::sin(WindDirectionRad);
            const f32 WindSpeed2 = WindSpeedCached * WindSpeedCached;
            const f32 L = WindSpeed2 / kGravity;
            const f32 L2 = std::max(L * L, 1e-6f);

            const f32 KDotW = Kx * WindX + Kz * WindZ;
            const f32 Directional = (KDotW * KDotW) / K2;
            const f32 SmallWaveDamping = std::exp(-K2 * 0.0001f);
            const f32 Base = std::exp(-1.0f / (K2 * L2)) / (K2 * K2);
            return std::max(Base * Directional * SmallWaveDamping, 0.0f);
        }

        void InitFourierAmps() {
            std::mt19937 Rng(1337u);
            std::normal_distribution<f32> Normal(0.0f, 1.0f);
            const f32 RecipSqrt2 = 1.0f / std::sqrt(2.0f);

            for (i32 y = 0; y < GridSize; ++y) {
                for (i32 x = 0; x < GridSize; ++x) {
                    const i32 Offset = GridOffset(x, y);
                    const Vec4& K = LUTK[Offset];
                    const f32 P = PhillipsSpectrum(K.X, K.Y);
                    const Complex Gaussian(Normal(Rng), Normal(Rng));
                    FourierAmps[Offset] = Gaussian * (RecipSqrt2 * std::sqrt(P));
                }
            }
        }

        void ComputeFFT1D(i32 Direction, f32* Real, f32* Imag) {
            const i32 N = GridSize;

            i32 J = 0;
            i32 I2 = N >> 1;
            for (i32 i = 0; i < N - 1; ++i) {
                if (i < J) {
                    std::swap(Real[i], Real[J]);
                    std::swap(Imag[i], Imag[J]);
                }

                i32 K = I2;
                while (K <= J) {
                    J -= K;
                    K >>= 1;
                }
                J += K;
            }

            f32 C1 = -1.0f;
            f32 C2 = 0.0f;
            i32 L2 = 1;

            for (i32 l = 0; l < LogGridSize; ++l) {
                const i32 L1 = L2;
                L2 <<= 1;
                f32 U1 = 1.0f;
                f32 U2 = 0.0f;

                for (J = 0; J < L1; ++J) {
                    for (i32 i = J; i < N; i += L2) {
                        const i32 I1 = i + L1;
                        const f32 T1 = U1 * Real[I1] - U2 * Imag[I1];
                        const f32 T2 = U1 * Imag[I1] + U2 * Real[I1];
                        Real[I1] = Real[i] - T1;
                        Imag[I1] = Imag[i] - T2;
                        Real[i] += T1;
                        Imag[i] += T2;
                    }

                    const f32 Z = U1 * C1 - U2 * C2;
                    U2 = U1 * C2 + U2 * C1;
                    U1 = Z;
                }

                C2 = std::sqrt((1.0f - C1) * 0.5f);
                if (Direction == 1) {
                    C2 = -C2;
                }
                C1 = std::sqrt((1.0f + C1) * 0.5f);
            }

            if (Direction == 1) {
                const f32 InvN = 1.0f / static_cast<f32>(N);
                for (i32 i = 0; i < N; ++i) {
                    Real[i] *= InvN;
                    Imag[i] *= InvN;
                }
            }
        }

        void ComputeFFT2D(i32 Direction, std::array<Complex, CellCount>& Data) {
            std::array<f32, GridSize> Real{};
            std::array<f32, GridSize> Imag{};

            for (i32 y = 0; y < GridSize; ++y) {
                for (i32 x = 0; x < GridSize; ++x) {
                    const Complex& C = Data[GridOffset(x, y)];
                    Real[x] = C.real();
                    Imag[x] = C.imag();
                }

                ComputeFFT1D(Direction, Real.data(), Imag.data());

                for (i32 x = 0; x < GridSize; ++x) {
                    Data[GridOffset(x, y)] = Complex(Real[x], Imag[x]);
                }
            }

            for (i32 x = 0; x < GridSize; ++x) {
                for (i32 y = 0; y < GridSize; ++y) {
                    const Complex& C = Data[GridOffset(x, y)];
                    Real[y] = C.real();
                    Imag[y] = C.imag();
                }

                ComputeFFT1D(Direction, Real.data(), Imag.data());

                for (i32 y = 0; y < GridSize; ++y) {
                    Data[GridOffset(x, y)] = Complex(Real[y], Imag[y]);
                }
            }
        }

        std::array<Vec4, CellCount> LUTK{};
        std::array<Vec4, CellCount> DisplacementGrid{};
        std::array<Complex, CellCount> FourierAmps{};
        std::array<Complex, CellCount> HeightField{};
        std::array<Complex, CellCount> DisplaceFieldX{};
        std::array<Complex, CellCount> DisplaceFieldZ{};

        f32 WindDirectionRad = 0.0f;
        f32 WindSpeedCached = 0.0f;
        f32 WaveSizeCached = 1.0f;
        bool Initialized = false;
    };

    void FOceanWater::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                 u32 SampleCount, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat) {
        if (Initialized) {
            return;
        }

        FFT = new FOceanFFT();

        BuildRootSignature(Device);
        BuildPSO(Device, SampleCount, RTFormat, DSFormat);
        CreateConstantBuffer(Device);
        CreateDisplacementTexture(Device, SRVHeap);
        CreateMesh(Device);

        Initialized = true;
        LogInfo("Oceano FFT inicializado");
    }

    void FOceanWater::Recreate(ID3D12Device* Device, u32 SampleCount,
                               DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat) {
        if (!Initialized) {
            return;
        }
        BuildPSO(Device, SampleCount, RTFormat, DSFormat);
    }

    void FOceanWater::Shutdown() {
        if (ConstantBuffer && MappedCB) {
            ConstantBuffer->Unmap(0, nullptr);
            MappedCB = nullptr;
        }
        delete FFT;
        FFT = nullptr;
        IndexBuffer.Reset();
        VertexBuffer.Reset();
        DisplacementUpload.Reset();
        DisplacementTexture.Reset();
        ConstantBuffer.Reset();
        PSO.Reset();
        RootSignature.Reset();
        Initialized = false;
    }

    void FOceanWater::SetWind(f32 DirectionRad, f32 Speed) {
        Settings.WindDirectionRad = DirectionRad;
        Settings.WindSpeed = std::max(Speed, 0.1f);
    }

    void FOceanWater::SetWaveParams(f32 WaveAmount, f32 WaveSize, f32 Choppiness) {
        Settings.WaveAmount = std::max(WaveAmount, 0.0f);
        Settings.WaveSize = std::max(WaveSize, 1.0f);
        Settings.Choppiness = std::max(Choppiness, 0.0f);
    }

    void FOceanWater::UpdatePerFrame(const Mat44& ViewProj, const Vec3& CameraPos,
                                     const Vec3& DirToSun, const Vec3& SunColor,
                                     f32 SunIntensity, f32 TimeSeconds,
                                     f32 IBLIntensity, f32 IBLRotation,
                                     f32 IBLMaxMip, bool IBLEnabled) {
        if (!Initialized || !Settings.Enabled || !MappedCB || !FFT) {
            return;
        }

        FFT->Update(TimeSeconds, Settings);

        const f32 Extent = std::max(Settings.SurfaceExtent, 1.0f);
        const f32 CellSize = Extent / static_cast<f32>(kMeshResolution - 1);
        const f32 GridX = std::floor(CameraPos.X / CellSize + 0.5f) * CellSize;
        const f32 GridZ = std::floor(CameraPos.Z / CellSize + 0.5f) * CellSize;
        const Vec3 SunN = DirToSun.NormalizedSafe(Vec3{ 0.0f, 0.7f, 0.7f }.Normalized());

        MappedCB->ViewProj = ViewProj;
        MappedCB->CameraPosition = { CameraPos.X, CameraPos.Y, CameraPos.Z, 1.0f };
        MappedCB->SunDirection = { SunN.X, SunN.Y, SunN.Z, SunIntensity };
        MappedCB->SunColor = { SunColor.X, SunColor.Y, SunColor.Z, 0.0f };
        MappedCB->WaterParams0 = {
            Settings.WaterLevelY,
            Settings.WaveAmount,
            std::max(Settings.WaveSize, 1.0f),
            Settings.Choppiness
        };
        MappedCB->WaterParams1 = {
            Extent,
            Settings.NormalScale,
            Settings.ReflectionScale,
            Settings.RefractionScale
        };
        MappedCB->WaterParams2 = {
            Settings.FoamAmount,
            Settings.FogDensity,
            TimeSeconds,
            0.0f
        };
        MappedCB->GridOrigin = { GridX, 0.0f, GridZ, CellSize };
        MappedCB->DeepColor = { Settings.DeepColor.X, Settings.DeepColor.Y, Settings.DeepColor.Z, 1.0f };
        MappedCB->ShallowColor = { Settings.ShallowColor.X, Settings.ShallowColor.Y, Settings.ShallowColor.Z, 1.0f };
        MappedCB->IBLParams = {
            IBLIntensity,
            IBLRotation,
            IBLMaxMip,
            IBLEnabled ? 1.0f : 0.0f
        };
    }

    void FOceanWater::Render(ID3D12GraphicsCommandList* CommandList,
                             FTextureSRVHeap& SRVHeap,
                             u32 IBLTableStart) {
        if (!Initialized || !Settings.Enabled || !CommandList || !FFT) {
            return;
        }

        UploadDisplacement(CommandList);

        CommandList->SetGraphicsRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(PSO.Get());
        CommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(DisplacementSRVSlot));
        CommandList->SetGraphicsRootDescriptorTable(2, SRVHeap.GpuHandle(IBLTableStart));

        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
        CommandList->IASetIndexBuffer(&IndexBufferView);
        CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);
    }

    void FOceanWater::BuildRootSignature(ID3D12Device* Device) {
        D3D12_DESCRIPTOR_RANGE DisplacementRange{};
        DisplacementRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DisplacementRange.NumDescriptors                    = 1;
        DisplacementRange.BaseShaderRegister                = 0;
        DisplacementRange.RegisterSpace                     = 0;
        DisplacementRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE IBLRange{};
        IBLRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        IBLRange.NumDescriptors                    = 3;
        IBLRange.BaseShaderRegister                = 1;
        IBLRange.RegisterSpace                     = 0;
        IBLRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[3]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &DisplacementRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &IBLRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].RegisterSpace    = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        Samplers[1]                  = Samplers[0];
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[1].ShaderRegister   = 1;
        Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob;
        Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob) {
                LogError(std::string("Ocean root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            }
            SMILE_HR(Hr);
        }
        SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(),
                                             Blob->GetBufferSize(),
                                             IID_PPV_ARGS(&RootSignature)));
    }

    void FOceanWater::BuildPSO(ID3D12Device* Device, u32 SampleCount,
                               DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat) {
        auto VS = LoadShader("OceanWater.vs_6_0.cso");
        auto PS = LoadShader("OceanWater.ps_6_0.cso");

        D3D12_INPUT_ELEMENT_DESC InputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode              = D3D12_FILL_MODE_SOLID;
        Raster.CullMode              = D3D12_CULL_MODE_NONE;
        Raster.FrontCounterClockwise = FALSE;
        Raster.DepthClipEnable       = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].BlendEnable           = FALSE;
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        Depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        Depth.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = RootSignature.Get();
        Desc.VS                    = { VS.data(), VS.size() };
        Desc.PS                    = { PS.data(), PS.size() };
        Desc.BlendState            = Blend;
        Desc.SampleMask            = UINT_MAX;
        Desc.RasterizerState       = Raster;
        Desc.DepthStencilState     = Depth;
        Desc.InputLayout           = { InputLayout, _countof(InputLayout) };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.NumRenderTargets      = 1;
        Desc.RTVFormats[0]         = RTFormat;
        Desc.DSVFormat             = DSFormat;
        Desc.SampleDesc            = { SampleCount, 0 };

        SMILE_HR(Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&PSO)));
    }

    void FOceanWater::CreateConstantBuffer(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = sizeof(OceanConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));
        std::memset(MappedCB, 0, sizeof(OceanConstants));
    }

    void FOceanWater::CreateDisplacementTexture(ID3D12Device* Device, FTextureSRVHeap& SRVHeap) {
        D3D12_RESOURCE_DESC TexDesc{};
        TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TexDesc.Width            = kDisplacementSize;
        TexDesc.Height           = kDisplacementSize;
        TexDesc.DepthOrArraySize = 1;
        TexDesc.MipLevels        = 1;
        TexDesc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
        TexDesc.SampleDesc       = { 1, 0 };
        TexDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        TexDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES DefaultHeap{};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        SMILE_HR(Device->CreateCommittedResource(
            &DefaultHeap, D3D12_HEAP_FLAG_NONE, &TexDesc, DisplacementState,
            nullptr, IID_PPV_ARGS(&DisplacementTexture)));

        DisplacementSRVSlot = SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                    = DXGI_FORMAT_R32G32B32A32_FLOAT;
        SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels       = 1;
        SRVDesc.Texture2D.MostDetailedMip = 0;
        SRVHeap.CreateSRV(Device, DisplacementTexture.Get(), SRVDesc, DisplacementSRVSlot);

        DisplacementRowPitch = static_cast<u32>(
            AlignUp(kDisplacementSize * sizeof(Vec4), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));

        D3D12_HEAP_PROPERTIES UploadHeap{};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC UploadDesc{};
        UploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        UploadDesc.Width            = DisplacementRowPitch * kDisplacementSize;
        UploadDesc.Height           = 1;
        UploadDesc.DepthOrArraySize = 1;
        UploadDesc.MipLevels        = 1;
        UploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
        UploadDesc.SampleDesc       = { 1, 0 };
        UploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(Device->CreateCommittedResource(
            &UploadHeap, D3D12_HEAP_FLAG_NONE, &UploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&DisplacementUpload)));
    }

    void FOceanWater::CreateMesh(ID3D12Device* Device) {
        std::vector<OceanVertex> Vertices;
        Vertices.reserve(kMeshResolution * kMeshResolution);

        for (u32 z = 0; z < kMeshResolution; ++z) {
            const f32 V = static_cast<f32>(z) / static_cast<f32>(kMeshResolution - 1);
            for (u32 x = 0; x < kMeshResolution; ++x) {
                const f32 U = static_cast<f32>(x) / static_cast<f32>(kMeshResolution - 1);
                OceanVertex Vertex{};
                Vertex.Position[0] = U - 0.5f;
                Vertex.Position[1] = 0.0f;
                Vertex.Position[2] = V - 0.5f;
                Vertex.UV[0] = U;
                Vertex.UV[1] = V;
                Vertices.push_back(Vertex);
            }
        }

        std::vector<u32> Indices;
        Indices.reserve((kMeshResolution - 1) * (kMeshResolution - 1) * 6);
        for (u32 z = 0; z < kMeshResolution - 1; ++z) {
            for (u32 x = 0; x < kMeshResolution - 1; ++x) {
                const u32 A = z * kMeshResolution + x;
                const u32 B = z * kMeshResolution + x + 1;
                const u32 C = (z + 1) * kMeshResolution + x;
                const u32 D = (z + 1) * kMeshResolution + x + 1;
                Indices.push_back(A);
                Indices.push_back(B);
                Indices.push_back(C);
                Indices.push_back(B);
                Indices.push_back(D);
                Indices.push_back(C);
            }
        }

        IndexCount = static_cast<u32>(Indices.size());

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const UINT VertexBufferSize = static_cast<UINT>(Vertices.size() * sizeof(OceanVertex));
        Desc.Width = VertexBufferSize;
        SMILE_HR(Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&VertexBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        void* Mapped = nullptr;
        SMILE_HR(VertexBuffer->Map(0, &NoRead, &Mapped));
        std::memcpy(Mapped, Vertices.data(), VertexBufferSize);
        VertexBuffer->Unmap(0, nullptr);

        VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VertexBufferView.StrideInBytes  = sizeof(OceanVertex);
        VertexBufferView.SizeInBytes    = VertexBufferSize;

        const UINT IndexBufferSize = static_cast<UINT>(Indices.size() * sizeof(u32));
        Desc.Width = IndexBufferSize;
        SMILE_HR(Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&IndexBuffer)));

        Mapped = nullptr;
        SMILE_HR(IndexBuffer->Map(0, &NoRead, &Mapped));
        std::memcpy(Mapped, Indices.data(), IndexBufferSize);
        IndexBuffer->Unmap(0, nullptr);

        IndexBufferView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
        IndexBufferView.SizeInBytes    = IndexBufferSize;
    }

    void FOceanWater::UploadDisplacement(ID3D12GraphicsCommandList* CommandList) {
        const auto& Displacements = FFT->GetDisplacements();

        u8* Mapped = nullptr;
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(DisplacementUpload->Map(0, &NoRead, reinterpret_cast<void**>(&Mapped)));
        for (u32 y = 0; y < kDisplacementSize; ++y) {
            const u8* Src = reinterpret_cast<const u8*>(Displacements.data() + y * kDisplacementSize);
            u8* Dst = Mapped + y * DisplacementRowPitch;
            std::memcpy(Dst, Src, kDisplacementSize * sizeof(Vec4));
        }
        DisplacementUpload->Unmap(0, nullptr);

        auto Transition = [&](D3D12_RESOURCE_STATES After) {
            if (DisplacementState == After) {
                return;
            }
            D3D12_RESOURCE_BARRIER Barrier{};
            Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barrier.Transition.pResource   = DisplacementTexture.Get();
            Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            Barrier.Transition.StateBefore = DisplacementState;
            Barrier.Transition.StateAfter  = After;
            CommandList->ResourceBarrier(1, &Barrier);
            DisplacementState = After;
        };

        Transition(D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION Src{};
        Src.pResource = DisplacementUpload.Get();
        Src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint.Offset = 0;
        Src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Src.PlacedFootprint.Footprint.Width    = kDisplacementSize;
        Src.PlacedFootprint.Footprint.Height   = kDisplacementSize;
        Src.PlacedFootprint.Footprint.Depth    = 1;
        Src.PlacedFootprint.Footprint.RowPitch = DisplacementRowPitch;

        D3D12_TEXTURE_COPY_LOCATION Dst{};
        Dst.pResource = DisplacementTexture.Get();
        Dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.SubresourceIndex = 0;

        CommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

        Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
}
