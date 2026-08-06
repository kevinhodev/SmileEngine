#include "Smile/Graphics/Atmosphere.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/DepthConfig.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>
#include <stdexcept>

namespace Smile {
    void FLut2D::Create(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                        DXGI_FORMAT _Format, u32 _Width, u32 _Height) {
        W = _Width; H = _Height; Fmt = _Format;
        State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = _Format;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, State, nullptr,
            IID_PPV_ARGS(&Resource)));
        VramTracker::Register(Resource.Get(), EVramCategory::Sky);

        SRVSlot = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                    = _Format;
        SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels       = 1;
        SRVDesc.Texture2D.MostDetailedMip = 0;
        _SRVHeap.CreateSRV(_Device, Resource.Get(), SRVDesc, SRVSlot);

        UAVSlot = _SRVHeap.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
        UAVDesc.Format             = _Format;
        UAVDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
        UAVDesc.Texture2D.MipSlice = 0;
        _SRVHeap.CreateUAV(_Device, Resource.Get(), UAVDesc, UAVSlot);
    }

    void FLut2D::Transition(ID3D12GraphicsCommandList* _CommandList,
                            D3D12_RESOURCE_STATES _After) {
        if (State == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Resource.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = State;
        B.Transition.StateAfter  = _After;
        _CommandList->ResourceBarrier(1, &B);
        State = _After;
    }

    void FAtmosphere::Initialize(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                 FUploadQueue& _UploadQueue, FTextureSRVHeap& _SRVHeap,
                                 DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        if (Initialized) return;
        SRVHeapPtr  = &_SRVHeap;
        SkyRTFormat = _RTFormat;
        SkyDSFormat = _DSFormat;

        CPUConstants.RayleighScattering = { 0.005802f, 0.013558f, 0.033100f, 8.0f  };
        CPUConstants.MieScattering      = { 0.003996f, 0.003996f, 0.003996f, 1.2f  };
        CPUConstants.MieExtinction      = { 0.004440f, 0.004440f, 0.004440f, 0.8f  };
        CPUConstants.OzoneAbsorption    = { 0.000650f, 0.001881f, 0.000085f, 0.0f  };
        CPUConstants.OzoneTent          = { 25.0f, 15.0f, 0.0f, 0.0f };
        CPUConstants.GroundAlbedo       = { 0.30f, 0.30f, 0.30f, 0.0f };
        CPUConstants.PlanetRadii        = { 6360.0f, 6460.0f, 0.0f, 0.0f };
        CPUConstants.SunDir             = { 0.0f, 0.6f, 0.8f, 1.0f };
        CPUConstants.AtmoSteps          = { 40.0f, 20.0f, 32.0f, 0.0f };
        CPUConstants.LutSize            = { (f32)kTransmittanceW, (f32)kTransmittanceH,
                                            (f32)kMultiScatterW,  (f32)kMultiScatterH };

        const f32 ViewHeight = CPUConstants.PlanetRadii.X + kPlanetRadiusOffsetKm;
        CPUConstants.SkyViewSize = { (f32)kSkyViewW, (f32)kSkyViewH, ViewHeight,
                                     kPlanetRadiusOffsetKm };

        const f32 SunDiskHalfAngleRad = 0.7f * 3.14159265358979f / 180.0f;
        CPUConstants.SunDisk          = { std::cos(SunDiskHalfAngleRad), 30.0f, 22.0f, 0.0f };
        CPUConstants.InvViewProjNoTrans = Mat44::Identity();
        CPUConstants.InvViewProj    = Mat44::Identity();
        CPUConstants.CameraWorldPos = { 0.0f, 0.0f, 0.0f, 0.001f };
        CPUConstants.AerialParams     = { 20.0f, (f32)kAerialSlices, 0.0f, 2.0f };
        CPUConstants.AerialVolumeSize = { (f32)kAerialW, (f32)kAerialH, 0.0f, 0.0f };
        CPUConstants.StarAxis       = { 0.0f, 1.0f, 0.0f, 0.0f };
        CPUConstants.NightSky       = { 0.0f, 0.0f, 0.0f, 0.0f };
        CPUConstants.ViewProjNoTrans = Mat44::Identity();
        CPUConstants.StarMatrix      = Mat44::Identity();
        CPUConstants.StarView        = { 1.0f, 1.0f, 1.0f, 1.0f };

        CreateConstantBuffer(_Device);

        Transmittance.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             kTransmittanceW, kTransmittanceH);
        MultiScatter.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            kMultiScatterW, kMultiScatterH);
        SkyView.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                       kSkyViewW, kSkyViewH);
        AerialPerspectiveVolume.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       kAerialW, kAerialH, kAerialSlices, 1, true);

        TransmittancePSO.Initialize(_Device, "BakeTransmittance.cs_6_0.cso", 1, 1);
        MultiScatterPSO.Initialize(_Device, "BakeMultiScatter.cs_6_0.cso", 1, 1);
        SkyViewPSO.Initialize(_Device, "BakeSkyView.cs_6_0.cso", 2, 1);
        AerialPerspectivePSO.Initialize(_Device, "BakeAerialPerspective.cs_6_0.cso", 2, 1);
        IntegrateAmbientPSO.Initialize(_Device, "IntegrateSkyAmbient.cs_6_0.cso", 1, 1);

        // Cube de reflexo da atmosfera (consumido pela água): raw + prefiltrado GGX.
        SkyReflRaw.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          kSkyReflSize, kSkyReflMips, true);
        SkyReflSpec.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           kSkyReflSize, kSkyReflMips, true);
        SkyReflBakePSO.Initialize(_Device, "BakeSkyReflection.cs_6_0.cso", 1, 1);
        SkyReflMipGenPSO.Initialize(_Device, "MipGen.cs_6_0.cso", true);
        SkyReflPrefilterPSO.Initialize(_Device, "SpecularPrefilter.cs_6_0.cso", true);

        // Buffer do ambient (2x float4) + readback ring p/ a CPU ler com latencia segura.
        {
            constexpr u64 kAmbientBytes = kAmbientVec4s * sizeof(f32) * 4;

            D3D12_HEAP_PROPERTIES DefHeap{};
            DefHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC BufDesc{};
            BufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            BufDesc.Width            = kAmbientBytes;
            BufDesc.Height           = 1;
            BufDesc.DepthOrArraySize = 1;
            BufDesc.MipLevels        = 1;
            BufDesc.Format           = DXGI_FORMAT_UNKNOWN;
            BufDesc.SampleDesc       = { 1, 0 };
            BufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            BufDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            SMILE_HR(_Device->CreateCommittedResource(
                &DefHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&AmbientBuffer)));
            VramTracker::Register(AmbientBuffer.Get(), EVramCategory::Sky);

            AmbientUAVSlot = _SRVHeap.Allocate(1);
            D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
            UAVDesc.Format                      = DXGI_FORMAT_UNKNOWN;
            UAVDesc.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
            UAVDesc.Buffer.NumElements          = kAmbientVec4s;
            UAVDesc.Buffer.StructureByteStride  = sizeof(f32) * 4;
            _SRVHeap.CreateUAV(_Device, AmbientBuffer.Get(), UAVDesc, AmbientUAVSlot);

            D3D12_HEAP_PROPERTIES RbHeap{};
            RbHeap.Type = D3D12_HEAP_TYPE_READBACK;
            BufDesc.Width = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * kAmbientBytes;
            BufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
            SMILE_HR(_Device->CreateCommittedResource(
                &RbHeap, D3D12_HEAP_FLAG_NONE, &BufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&AmbientReadback)));
            D3D12_RANGE All{ 0, static_cast<SIZE_T>(BufDesc.Width) };
            void* Ptr = nullptr;
            SMILE_HR(AmbientReadback->Map(0, &All, &Ptr));
            AmbientMapped = reinterpret_cast<u8*>(Ptr);
            std::memset(AmbientMapped, 0, static_cast<size_t>(BufDesc.Width));
        }

        MoonTexture = FTexture::CreateDefault(_Device, _UploadQueue, _SRVHeap, EDefaultTexture::White);

        BuildInputTables(_Device, _SRVHeap);
        BuildSkyRootSignature(_Device);
        BuildSkyPSO(_Device, _RTFormat, _DSFormat);

        Dirty       = true;
        Initialized = true;
        BakeIfDirty(_Device, _CmdQueue); 
        LogDebug("Atmosfera (Hillaire) inicializada: Transmittance + MultiScatter + SkyView");
    }

    void FAtmosphere::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(AtmosphereConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &Ptr));
        MappedBase = reinterpret_cast<u8*>(Ptr);

        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedBase + static_cast<size_t>(i) * sizeof(AtmosphereConstants),
                        &CPUConstants, sizeof(AtmosphereConstants));
    }

    void FAtmosphere::BuildInputTables(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        SkyViewBakeTableStart = _SRVHeap.Allocate(2);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(SkyViewBakeTableStart);
            D3D12_CPU_DESCRIPTOR_HANDLE Srcs[2] = {
                _SRVHeap.CpuHandleStaging(Transmittance.SRVSlot),
                _SRVHeap.CpuHandleStaging(MultiScatter.SRVSlot),
            };
            UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Srcs, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        SkyRenderTableStart = _SRVHeap.Allocate(3);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(SkyRenderTableStart);
            D3D12_CPU_DESCRIPTOR_HANDLE Srcs[3] = {
                _SRVHeap.CpuHandleStaging(SkyView.SRVSlot),
                _SRVHeap.CpuHandleStaging(Transmittance.SRVSlot),
                _SRVHeap.CpuHandleStaging(MoonTexture.SRVSlot()),
            };
            UINT DstCount = 3; UINT SrcCounts[3] = { 1, 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &DstCount, 3, Srcs, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    void FAtmosphere::LoadMoonTexture(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                                      FTextureSRVHeap& _SRVHeap, const std::wstring& _Path) {
        if (!Initialized) return;
        // LROC color e uma imagem de cor sRGB. O flag tambem faz a cadeia de mips ser filtrada
        // em linear; o SRV sRGB devolve albedo linear ao shader sem pow manual.
        FTexture Tex = FTexture::LoadFromFile(_Device, _UploadQueue, _SRVHeap,
                                              _Path, false, true);
        if (!Tex.IsValid()) return;

        MoonTexture.Release(_SRVHeap);
        MoonTexture   = std::move(Tex);
        MoonTexLoaded = true;

        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(SkyRenderTableStart + 2);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(MoonTexture.SRVSlot());
        UINT DstCount = 1, SrcCount = 1;
        _Device->CopyDescriptors(1, &Dst, &DstCount, 1, &Src, &SrcCount,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FAtmosphere::LoadStarCatalog(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                      const std::wstring& _Path) {
        if (!Initialized) return;

        struct FStarRec { f32 X, Y, Z, Brightness; u32 Color; };
        static_assert(sizeof(FStarRec) == 20, "layout do stars.sstars");

        std::ifstream File(_Path, std::ios::binary);
        if (!File) { LogDebug("Catalogo de estrelas nao encontrado; hash procedural segue"); return; }

        u32 Header[4]{};
        File.read(reinterpret_cast<char*>(Header), sizeof(Header));
        if (!File || Header[0] != 0x52545353u /*'SSTR'*/ || Header[1] != 1u || Header[2] == 0) {
            LogWarning("Catalogo de estrelas invalido (magic/versao)");
            return;
        }
        const u32 Count = Header[2];
        std::vector<FStarRec> Stars(Count);
        File.read(reinterpret_cast<char*>(Stars.data()),
                  static_cast<std::streamsize>(Count * sizeof(FStarRec)));
        if (!File) { LogWarning("Catalogo de estrelas truncado"); return; }

        // Upload heap direto: 8k estrelas x 20B, lido 1x por frame so a noite — nao vale copy.
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(Count) * sizeof(FStarRec);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&StarBuffer)));
        {
            D3D12_RANGE NoRead{ 0, 0 };
            void* Ptr = nullptr;
            SMILE_HR(StarBuffer->Map(0, &NoRead, &Ptr));
            std::memcpy(Ptr, Stars.data(), static_cast<size_t>(Desc.Width));
            StarBuffer->Unmap(0, nullptr);
        }

        const u32 StarSRVSlot = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                     = DXGI_FORMAT_UNKNOWN;
        SRVDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        SRVDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Buffer.NumElements         = Count;
        SRVDesc.Buffer.StructureByteStride = sizeof(FStarRec);
        _SRVHeap.CreateSRV(_Device, StarBuffer.Get(), SRVDesc, StarSRVSlot);

        // Tabela do passe: [t0 = estrelas (VS), t1 = transmitancia (PS)].
        StarTableStart = _SRVHeap.Allocate(2);
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(StarTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE Srcs[2] = {
            _SRVHeap.CpuHandleStaging(StarSRVSlot),
            _SRVHeap.CpuHandleStaging(Transmittance.SRVSlot),
        };
        UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
        _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Srcs, SrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        BuildStarPipeline(_Device, SkyRTFormat, SkyDSFormat);

        StarCount = Count;
        CPUConstants.NightSky.Z = 1.0f; // desliga o hash procedural do sky pass
        LogDebug("Catalogo de estrelas: " + std::to_string(Count) + " estrelas (Yale BSC)");
    }

    void FAtmosphere::BuildStarPipeline(ID3D12Device* _Device,
                                        DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        {
            D3D12_DESCRIPTOR_RANGE SRVRange{};
            SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            SRVRange.NumDescriptors                    = 2;
            SRVRange.BaseShaderRegister                = 0;
            SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            D3D12_ROOT_PARAMETER RootParams[2]{};
            RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            RootParams[0].Descriptor.ShaderRegister = 0;
            RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
            RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
            RootParams[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
            RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_STATIC_SAMPLER_DESC Sampler{};
            Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            Sampler.MaxAnisotropy    = 1;
            Sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
            Sampler.MaxLOD           = D3D12_FLOAT32_MAX;
            Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC Desc{};
            Desc.NumParameters     = _countof(RootParams);
            Desc.pParameters       = RootParams;
            Desc.NumStaticSamplers = 1;
            Desc.pStaticSamplers   = &Sampler;

            Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
            HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                     &Blob, &ErrorBlob);
            if (FAILED(Hr)) {
                if (ErrorBlob)
                    LogError(std::string("Star root sig error: ") +
                             static_cast<const char*>(ErrorBlob->GetBufferPointer()));
                SMILE_HR(Hr);
            }
            SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(),
                                                  Blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&StarRootSig)));
        }

        auto VS = LoadShaderBytecode("StarField.vs_6_0.cso");
        auto PS = LoadShaderBytecode("StarField.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        // Aditivo sobre o HDR: estrela soma na cor do ceu; geometria oculta via depth-test.
        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].BlendEnable           = TRUE;
        Blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ONE;
        Blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        Depth.DepthFunc      = kDepthFuncLessEqual;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = StarRootSig.Get();
        Desc.VS                    = { VS.data(), VS.size() };
        Desc.PS                    = { PS.data(), PS.size() };
        Desc.BlendState            = Blend;
        Desc.SampleMask            = UINT_MAX;
        Desc.RasterizerState       = Raster;
        Desc.DepthStencilState     = Depth;
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.NumRenderTargets      = 1;
        Desc.RTVFormats[0]         = _RTFormat;
        Desc.DSVFormat             = _DSFormat;
        Desc.SampleDesc            = { 1, 0 };
        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&StarPSO)));
    }

    void FAtmosphere::RenderStars(ID3D12GraphicsCommandList* _CommandList,
                                  FTextureSRVHeap& _SRVHeap) {
        if (!Initialized || StarCount == 0 || !StarPSO) return;
        _CommandList->SetGraphicsRootSignature(StarRootSig.Get());
        _CommandList->SetPipelineState(StarPSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, CBAddr());
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(StarTableStart));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(6, StarCount, 0, 0);
    }

    void FAtmosphere::BuildSkyRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 3; 
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[2]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; 
        RootParams[0].Descriptor.RegisterSpace  = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        RootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        RootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].RegisterSpace    = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        Samplers[1]                  = Samplers[0];
        Samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        Samplers[1].ShaderRegister   = 1;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("Sky root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&SkyRootSig)));
    }

    void FAtmosphere::BuildSkyPSO(ID3D12Device* _Device,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        auto VS = LoadShaderBytecode("SkyAtmosphere.vs_6_0.cso");
        auto PS = LoadShaderBytecode("SkyAtmosphere.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        Depth.DepthFunc      = kDepthFuncLessEqual; 
        Depth.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
        Desc.pRootSignature        = SkyRootSig.Get();
        Desc.VS                    = { VS.data(), VS.size() };
        Desc.PS                    = { PS.data(), PS.size() };
        Desc.BlendState            = Blend;
        Desc.SampleMask            = UINT_MAX;
        Desc.RasterizerState       = Raster;
        Desc.DepthStencilState     = Depth;
        Desc.InputLayout           = { nullptr, 0 };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.NumRenderTargets      = 1;
        Desc.RTVFormats[0]         = _RTFormat;
        Desc.DSVFormat             = _DSFormat;
        Desc.SampleDesc            = { 1, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&SkyPSO)));
    }

    void FAtmosphere::RecreateSky(ID3D12Device* _Device,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        if (!Initialized) return;
        BuildSkyPSO(_Device, _RTFormat, _DSFormat);
        // O cubemap atmosferico e a fonte ambiental da agua. Mante-lo fora do hot reload
        // fazia alteracoes no horizonte parecerem inertes ate reiniciar o editor.
        SkyReflBakePSO.Initialize(_Device, "BakeSkyReflection.cs_6_0.cso", 1, 1);
    }

    void FAtmosphere::UpdatePerFrame(u32 _FrameSlot, const Vec3& _DirToSun,
                                     const Mat44& _InvViewProjNoTranslation,
                                     const Mat44& _ViewProjNoTranslation,
                                     const Mat44& _InvViewProjFull, const Vec3& _CameraWorldPos,
                                     f32 _KmPerWorldUnit, f32 _RenderW, f32 _RenderH,
                                     f32 _OutputW, f32 _OutputH) {
        FrameSlot = _FrameSlot;
        Vec3 d = _DirToSun.NormalizedSafe(Vec3{ 0.0f, 0.6f, 0.8f }.Normalized());
        CPUConstants.SunDir = { d.X, d.Y, d.Z, CPUConstants.SunDir.W };
        CPUConstants.InvViewProjNoTrans = _InvViewProjNoTranslation;
        CPUConstants.ViewProjNoTrans = _ViewProjNoTranslation;
        CPUConstants.InvViewProj    = _InvViewProjFull;
        CPUConstants.CameraWorldPos = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z, _KmPerWorldUnit };
        const f32 SafeOutputW = std::max(_OutputW, 1.0f);
        const f32 SafeOutputH = std::max(_OutputH, 1.0f);
        CPUConstants.StarView = { SafeOutputW, SafeOutputH,
                                  std::max(_RenderW, 1.0f) / SafeOutputW,
                                  std::max(_RenderH, 1.0f) / SafeOutputH };

        // View height dinamico: sky-view LUT, transmitancia do disco/estrelas e ambient seguem a
        // altitude real da camera sobre o offset numerico do planeta (UE recalcula por frame).
        //
        // Este e o UNICO view height da engine: o bake do aerial perspective e o caminho de
        // GI/reflexoes leem daqui (ViewHeightKm()). Ele decide o angulo do horizonte, logo onde
        // cai a dobra do warp em v=0.5 do sky-view LUT — dois consumidores com valores
        // diferentes leem texels diferentes para a MESMA direcao, bem na banda de maior
        // gradiente. Era o caso: o caminho de GI tinha 6360.5 hardcoded (0,72 grau de dip
        // contra os 0,032 do bake, ~3,8 texels dos 104).
        //
        // O piso do clamp e o proprio offset numerico. Ele era 0.05 km, residuo de quando o
        // offset nominal valia 0.5 km, e sozinho ja recriava 49 m de divergencia contra o bake.
        // Nao protege de nada: o max(CameraWorldPos.Y, 0) abaixo garante o limite inferior.
        CPUConstants.SkyViewSize.Z = std::clamp(
            CPUConstants.PlanetRadii.X + kPlanetRadiusOffsetKm +
                std::max(_CameraWorldPos.Y, 0.0f) * _KmPerWorldUnit,
            CPUConstants.PlanetRadii.X + kPlanetRadiusOffsetKm,
            CPUConstants.PlanetRadii.Y - 1.0f);

        if (MappedBase) *Mapped() = CPUConstants;
    }

    Vec3 FAtmosphere::SunTransmittance(const Vec3& _DirToSun) const {
        if (!Initialized) return Vec3{ 1.0f, 1.0f, 1.0f }; 

        const AtmosphereConstants& C = CPUConstants;
        const f32 bottomR    = C.PlanetRadii.X;
        const f32 topR       = C.PlanetRadii.Y;
        const f32 viewHeight = C.SkyViewSize.Z; // segue a altitude da camera (view height dinamico)

        const f32 cosZ = _DirToSun.Y;
        if (cosZ <= 0.0f) return Vec3{ 0.0f, 0.0f, 0.0f };
        const f32 sinZ = std::sqrt(std::max(0.0f, 1.0f - cosZ * cosZ));

        const f32 b    = viewHeight * cosZ;
        const f32 c    = viewHeight * viewHeight - topR * topR;
        const f32 disc = b * b - c;
        if (disc < 0.0f) return Vec3{ 1.0f, 1.0f, 1.0f };
        const f32 tMax = -b + std::sqrt(disc);
        if (tMax <= 0.0f) return Vec3{ 1.0f, 1.0f, 1.0f };

        const int steps = std::max(2, (int)C.AtmoSteps.X);
        const f32 dt    = tMax / (f32)steps;

        f32 odR = 0.0f, odG = 0.0f, odB = 0.0f;
        for (int i = 0; i < steps; ++i) {
            const f32 t  = (i + 0.5f) * dt;
            const f32 px = sinZ * t;
            const f32 py = viewHeight + cosZ * t;
            const f32 altitude = std::sqrt(px * px + py * py) - bottomR;

            const f32 densR = std::exp(-altitude / C.RayleighScattering.W);
            const f32 densM = std::exp(-altitude / C.MieScattering.W);
            const f32 densO = std::max(0.0f, 1.0f - std::abs(altitude - C.OzoneTent.X) / C.OzoneTent.Y);

            odR += (C.RayleighScattering.X * densR + C.MieExtinction.X * densM + C.OzoneAbsorption.X * densO) * dt;
            odG += (C.RayleighScattering.Y * densR + C.MieExtinction.Y * densM + C.OzoneAbsorption.Y * densO) * dt;
            odB += (C.RayleighScattering.Z * densR + C.MieExtinction.Z * densM + C.OzoneAbsorption.Z * densO) * dt;
        }
        return Vec3{ std::exp(-odR), std::exp(-odG), std::exp(-odB) };
    }

    void FAtmosphere::SetNightParams(const Vec3& _DirToMoon, f32 _CosDiskRadius, f32 _DiskBrightness,
                                     f32 _StarIntensity, f32 _NightFactor, f32 _TimeSec) {
        CPUConstants.MoonDir    = { _DirToMoon.X, _DirToMoon.Y, _DirToMoon.Z, _CosDiskRadius };
        CPUConstants.MoonParams = { _DiskBrightness, _StarIntensity, _NightFactor, _TimeSec };
    }

    void FAtmosphere::SetStarRotation(const Vec3& _PoleAxis, f32 _AngleRad) {
        const Vec3 A = _PoleAxis.NormalizedSafe(Vec3{ 0.0f, 1.0f, 0.0f });
        CPUConstants.StarAxis = { A.X, A.Y, A.Z, _AngleRad };

        // Matriz catalogo->mundo p/ o passe de estrelas: +Y do catalogo = polo celeste; X/Z
        // giram em torno do polo com o tempo sideral (rotacao = -angulo, mesmo sentido do
        // campo hash: estrelas nascem no leste).
        Vec3 X0 = Vec3{ 0.0f, 1.0f, 0.0f }.Cross(A);
        X0 = X0.NormalizedSafe(Vec3{ 1.0f, 0.0f, 0.0f });
        const Vec3 Z0 = X0.Cross(A);

        const f32 c = std::cos(-_AngleRad), s = std::sin(-_AngleRad);
        auto RotAroundPole = [&](const Vec3& V) { // Rodrigues; V perpendicular a A
            const Vec3 AxV = A.Cross(V);
            return Vec3{ V.X * c + AxV.X * s, V.Y * c + AxV.Y * s, V.Z * c + AxV.Z * s };
        };
        const Vec3 Xr = RotAroundPole(X0);
        const Vec3 Zr = RotAroundPole(Z0);

        Mat44 M = Mat44::Identity();
        M.M[0][0] = Xr.X; M.M[0][1] = Xr.Y; M.M[0][2] = Xr.Z; // linha 0: catalogo +X
        M.M[1][0] = A.X;  M.M[1][1] = A.Y;  M.M[1][2] = A.Z;  // linha 1: catalogo +Y (polo)
        M.M[2][0] = Zr.X; M.M[2][1] = Zr.Y; M.M[2][2] = Zr.Z; // linha 2: catalogo +Z
        CPUConstants.StarMatrix = M;
    }

    void FAtmosphere::SetMoonSkyLight(f32 _SkyIllumScale, f32 _CoronaIntensity) {
        CPUConstants.NightSky.X = CPUConstants.SunDisk.Z * std::max(_SkyIllumScale, 0.0f);
        CPUConstants.NightSky.Y = std::max(_CoronaIntensity, 0.0f);
    }

    void FAtmosphere::SetSunDiskHalfAngle(f32 _DegHalfAngle) {
        CPUConstants.SunDisk.X = std::cos(_DegHalfAngle * 3.14159265358979f / 180.0f);
    }

    void FAtmosphere::SetSunGlare(f32 _Intensity) {
        CPUConstants.SunDisk.W = _Intensity;
    }

    f32 FAtmosphere::GetSunDiskHalfAngle() const {
        constexpr f32 kRadiansToDegrees = 180.0f / 3.14159265358979f;
        return static_cast<f32>(std::acos(CPUConstants.SunDisk.X) * kRadiansToDegrees);
    }

    f32 FAtmosphere::GetSunGlare() const {
        return CPUConstants.SunDisk.W;
    }

    void FAtmosphere::BakeIfDirty(ID3D12Device* _Device, FCommandQueue& _CmdQueue) {
        if (!Initialized || !Dirty) return;
        Bake(_Device, _CmdQueue);
        Dirty = false;
    }

    // Mesmo bake das duas LUTs invariantes ao frame, mas GRAVADO na command list do frame.
    //
    // O Bake() abaixo faz ResetForRecording + ExecuteAndSync — flush completo de GPU, que so
    // pode acontecer fora do frame (por isso ele so era chamado de dentro do Initialize). O
    // resultado pratico era que MarkDirty() nao tinha efeito NENHUM: nao havia caminho que
    // consumisse o flag depois da inicializacao. Um slider de parametro fisico (coeficientes,
    // raio do planeta, albedo do solo) seria no-op silencioso.
    void FAtmosphere::RecordBakeIfDirty(ID3D12GraphicsCommandList* _CommandList) {
        if (!Initialized || !Dirty || !_CommandList) return;
        if (MappedBase) *Mapped() = CPUConstants;
        const D3D12_GPU_VIRTUAL_ADDRESS CB = CBAddr();

        Transmittance.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransmittancePSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CB);
        _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(Transmittance.SRVSlot));
        _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(Transmittance.UAVSlot));
        _CommandList->Dispatch((kTransmittanceW + 7) / 8, (kTransmittanceH + 7) / 8, 1);
        Transmittance.Transition(_CommandList, kReadState);

        MultiScatter.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        MultiScatterPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CB);
        _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(Transmittance.SRVSlot));
        _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(MultiScatter.UAVSlot));
        _CommandList->Dispatch((kMultiScatterW + 7) / 8, (kMultiScatterH + 7) / 8, 1);
        MultiScatter.Transition(_CommandList, kReadState);

        Dirty = false;
    }

    void FAtmosphere::Bake(ID3D12Device* _Device, FCommandQueue& _CmdQueue) {
        (void)_Device;
        if (MappedBase) *Mapped() = CPUConstants;

        _CmdQueue.ResetForRecording();
        auto* CL = _CmdQueue.List();

        ID3D12DescriptorHeap* Heaps[] = { SRVHeapPtr->Native() };
        CL->SetDescriptorHeaps(1, Heaps);

        const D3D12_GPU_VIRTUAL_ADDRESS CBAddr = this->CBAddr();

        Transmittance.Transition(CL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransmittancePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr);
        CL->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(Transmittance.SRVSlot));
        CL->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(Transmittance.UAVSlot));
        CL->Dispatch((kTransmittanceW + 7) / 8, (kTransmittanceH + 7) / 8, 1);
        Transmittance.Transition(CL, kReadState);

        MultiScatter.Transition(CL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        MultiScatterPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr);
        CL->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(Transmittance.SRVSlot));
        CL->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(MultiScatter.UAVSlot));
        CL->Dispatch((kMultiScatterW + 7) / 8, (kMultiScatterH + 7) / 8, 1);
        MultiScatter.Transition(CL, kReadState);

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL };
        _CmdQueue.ExecuteAndSync(Lists, 1);
    }

    void FAtmosphere::RecordSkyViewBake(ID3D12GraphicsCommandList* _CommandList) {
        if (!Initialized) return;

        SkyView.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SkyViewPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(SkyViewBakeTableStart));
        _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(SkyView.UAVSlot));
        _CommandList->Dispatch((kSkyViewW + 7) / 8, (kSkyViewH + 7) / 8, 1);
        SkyView.Transition(_CommandList, kReadState);
    }

    void FAtmosphere::RecordSkyReflectionBake(ID3D12GraphicsCommandList* _CommandList) {
        if (!Initialized || !SkyReflRaw.IsValid()) return;

        // 1) SkyView LUT -> mip0 do cube raw (SkyView ja esta em kReadState, o
        //    RecordSkyViewBake roda antes no mesmo command list).
        SkyReflRaw.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SkyReflBakePSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(SkyView.SRVSlot));
        _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(SkyReflRaw.UAVSlot(0)));
        _CommandList->Dispatch((kSkyReflSize + 7) / 8, (kSkyReflSize + 7) / 8, 6);
        SkyReflRaw.TransitionMip(_CommandList, 0, kReadState);

        // 2) Mip chain do raw (o prefilter seleciona mip da fonte pelo pdf).
        for (u32 Mip = 1; Mip < kSkyReflMips; ++Mip) {
            const u32 MipSize = kSkyReflSize >> Mip;
            const u32 Constants[2] = { MipSize, Mip - 1 };
            SkyReflMipGenPSO.Bind(_CommandList);
            _CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);
            _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(SkyReflRaw.SRVSlot()));
            _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(SkyReflRaw.UAVSlot(Mip)));
            const u32 Groups = (MipSize + 7) / 8;
            _CommandList->Dispatch(Groups, Groups, 6);
            SkyReflRaw.TransitionMip(_CommandList, Mip, kReadState);
        }

        // 3) Prefilter GGX raw -> spec (roughness = mip/(mips-1), igual ao chain do HDRI).
        SkyReflSpec.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (u32 Mip = 0; Mip < kSkyReflMips; ++Mip) {
            const u32 MipSize = kSkyReflSize >> Mip;
            const f32 Roughness = static_cast<f32>(Mip) / static_cast<f32>(kSkyReflMips - 1);
            u32 Constants[4];
            Constants[0] = MipSize;
            Constants[1] = kSkyReflSize;
            std::memcpy(&Constants[2], &Roughness, sizeof(f32));
            Constants[3] = kSkyReflSamples;
            SkyReflPrefilterPSO.Bind(_CommandList);
            _CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);
            _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(SkyReflRaw.SRVSlot()));
            _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(SkyReflSpec.UAVSlot(Mip)));
            const u32 Groups = (MipSize + 7) / 8;
            _CommandList->Dispatch(Groups, Groups, 6);
        }
        SkyReflSpec.Transition(_CommandList, kReadState);
    }

    void FAtmosphere::RecordSkyAmbientIntegration(ID3D12GraphicsCommandList* _CommandList) {
        if (!Initialized || !AmbientBuffer) return;

        // SkyView ja esta em kReadState (RecordSkyViewBake roda antes no mesmo command list).
        IntegrateAmbientPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(SkyView.SRVSlot));
        _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(AmbientUAVSlot));
        _CommandList->Dispatch(1, 1, 1);

        constexpr u64 kAmbientBytes = kAmbientVec4s * sizeof(f32) * 4;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = AmbientBuffer.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        _CommandList->ResourceBarrier(1, &B);

        _CommandList->CopyBufferRegion(AmbientReadback.Get(),
                                       static_cast<UINT64>(FrameSlot) * kAmbientBytes,
                                       AmbientBuffer.Get(), 0, kAmbientBytes);

        std::swap(B.Transition.StateBefore, B.Transition.StateAfter);
        _CommandList->ResourceBarrier(1, &B);

        ++AmbientRecorded;
    }

    bool FAtmosphere::GetSkyAmbient(u32 _FrameSlot, Vec3& _OutSky, Vec3& _OutGround) const {
        // O slot _FrameSlot foi escrito ha kFramesInFlight frames e a fence dele ja foi esperada
        // (CommandQueue::BeginFrame) — leitura segura, latencia invisivel p/ ambient.
        if (!AmbientMapped || AmbientRecorded < FCommandQueue::kFramesInFlight) return false;
        constexpr size_t kAmbientBytes = kAmbientVec4s * sizeof(f32) * 4;
        const f32* P = reinterpret_cast<const f32*>(AmbientMapped + _FrameSlot * kAmbientBytes);
        _OutSky    = Vec3{ P[0], P[1], P[2] };
        _OutGround = Vec3{ P[4], P[5], P[6] };
        return true;
    }

    bool FAtmosphere::GetSkyAmbientSH(u32 _FrameSlot, Vec4 _OutSH[3]) const {
        if (!AmbientMapped || AmbientRecorded < FCommandQueue::kFramesInFlight) return false;
        constexpr size_t kAmbientBytes = kAmbientVec4s * sizeof(f32) * 4;
        const f32* P = reinterpret_cast<const f32*>(AmbientMapped + _FrameSlot * kAmbientBytes);
        for (u32 c = 0; c < 3; ++c) {
            const f32* V = P + (2 + c) * 4; // [0]/[1] sao as 2 cores; a SH comeca em [2]
            _OutSH[c] = Vec4{ V[0], V[1], V[2], V[3] };
        }
        return true;
    }

    void FAtmosphere::RecordAerialPerspectiveBake(ID3D12GraphicsCommandList* _CommandList) {
        if (!Initialized || !AerialPerspectiveVolume.IsValid()) return;

        AerialPerspectiveVolume.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        AerialPerspectivePSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(SkyViewBakeTableStart));
        _CommandList->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(AerialPerspectiveVolume.UAVSlot(0)));
        _CommandList->Dispatch((kAerialW + 3) / 4, (kAerialH + 3) / 4, (kAerialSlices + 3) / 4);
        AerialPerspectiveVolume.Transition(_CommandList, kReadState);
    }

    void FAtmosphere::RenderSky(ID3D12GraphicsCommandList* _CommandList, FTextureSRVHeap& _SRVHeap) {
        if (!Initialized) return;
        _CommandList->SetGraphicsRootSignature(SkyRootSig.Get());
        _CommandList->SetPipelineState(SkyPSO.Get());
        _CommandList->SetGraphicsRootConstantBufferView(0, CBAddr());
        _CommandList->SetGraphicsRootDescriptorTable(1, _SRVHeap.GpuHandle(SkyRenderTableStart));
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 0, nullptr);
        _CommandList->IASetIndexBuffer(nullptr);
        _CommandList->DrawInstanced(3, 1, 0, 0);
    }
}
