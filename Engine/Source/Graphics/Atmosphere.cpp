#include "Smile/Graphics/Atmosphere.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>
#include <stdexcept>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    namespace {
        std::vector<u8> LoadShader(const std::string& _Name) {
            const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + _Name;
            std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
            if (!File) {
                LogError("Falha ao abrir shader atmosfera: " + FullPath);
                throw std::runtime_error("Atmosphere shader nao encontrado: " + FullPath);
            }
            const auto Size = static_cast<size_t>(File.tellg());
            std::vector<u8> Data(Size);
            File.seekg(0);
            File.read(reinterpret_cast<char*>(Data.data()), Size);
            return Data;
        }
    }

    // ---- FLut2D ------------------------------------------------------------
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

    // ---- FAtmosphere -------------------------------------------------------
    void FAtmosphere::Initialize(ID3D12Device* _Device, FCommandQueue& _CmdQueue,
                                 FTextureSRVHeap& _SRVHeap, u32 _SampleCount,
                                 DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        if (Initialized) return;
        SRVHeapPtr = &_SRVHeap;

        // Default Earth-like atmosphere (Hillaire / Bruneton values, km & km^-1).
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

        const f32 ViewHeight = CPUConstants.PlanetRadii.X + kGroundAltitudeKm;
        CPUConstants.SkyViewSize = { (f32)kSkyViewW, (f32)kSkyViewH, ViewHeight, kGroundAltitudeKm };

        // Sun disk + glare. Disk ~0.7 deg half-angle (slightly above the physical
        // ~0.27 deg for visibility); the wide soft glare (w) gives the game-like
        // "big sun" look without a bloom post-process. Tuned in the editor later.
        const f32 SunDiskHalfAngleRad = 0.7f * 3.14159265358979f / 180.0f;
        CPUConstants.SunDisk          = { std::cos(SunDiskHalfAngleRad), 30.0f, 22.0f, 4.0f };
        CPUConstants.InvViewProjNoTrans = Mat44::Identity();

        CreateConstantBuffer(_Device);

        Transmittance.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             kTransmittanceW, kTransmittanceH);
        MultiScatter.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            kMultiScatterW, kMultiScatterH);
        SkyView.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                       kSkyViewW, kSkyViewH);

        TransmittancePSO.Initialize(_Device, "BakeTransmittance.cs_6_0.cso", 1, 1);
        MultiScatterPSO.Initialize(_Device, "BakeMultiScatter.cs_6_0.cso", 1, 1);
        SkyViewPSO.Initialize(_Device, "BakeSkyView.cs_6_0.cso", 2, 1);

        BuildInputTables(_Device, _SRVHeap);
        BuildSkyRootSignature(_Device);
        BuildSkyPSO(_Device, _SampleCount, _RTFormat, _DSFormat);

        Dirty       = true;
        Initialized = true;
        BakeIfDirty(_Device, _CmdQueue); // transmittance + multi-scatter (once)
        LogInfo("Atmosfera (Hillaire) inicializada: Transmittance + MultiScatter + SkyView");
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
        // Popula todas as regioes para que qualquer slot seja valido antes do 1o UpdatePerFrame.
        for (u32 i = 0; i < FCommandQueue::kFramesInFlight; ++i)
            std::memcpy(MappedBase + static_cast<size_t>(i) * sizeof(AtmosphereConstants),
                        &CPUConstants, sizeof(AtmosphereConstants));
    }

    void FAtmosphere::BuildInputTables(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        // Sky-view bake input: [transmittance(t0), multiscatter(t1)].
        SkyViewBakeTableStart = _SRVHeap.Allocate(2);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(SkyViewBakeTableStart);
            D3D12_CPU_DESCRIPTOR_HANDLE Srcs[2] = {
                _SRVHeap.CpuHandle(Transmittance.SRVSlot),
                _SRVHeap.CpuHandle(MultiScatter.SRVSlot),
            };
            UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Srcs, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        // Sky render input: [skyview(t0), transmittance(t1)].
        SkyRenderTableStart = _SRVHeap.Allocate(2);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(SkyRenderTableStart);
            D3D12_CPU_DESCRIPTOR_HANDLE Srcs[2] = {
                _SRVHeap.CpuHandle(SkyView.SRVSlot),
                _SRVHeap.CpuHandle(Transmittance.SRVSlot),
            };
            UINT DstCount = 2; UINT SrcCounts[2] = { 1, 1 };
            _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Srcs, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    void FAtmosphere::BuildSkyRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 2; // t0 = skyview, t1 = transmittance
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.RegisterSpace                     = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[2]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0; // b0
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

    void FAtmosphere::BuildSkyPSO(ID3D12Device* _Device, u32 _SampleCount,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        auto VS = LoadShader("SkyAtmosphere.vs_6_0.cso");
        auto PS = LoadShader("SkyAtmosphere.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        Depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
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
        Desc.SampleDesc            = { _SampleCount, 0 };

        SMILE_HR(_Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&SkyPSO)));
    }

    void FAtmosphere::RecreateSky(ID3D12Device* _Device, u32 _SampleCount,
                                  DXGI_FORMAT _RTFormat, DXGI_FORMAT _DSFormat) {
        if (!Initialized) return;
        BuildSkyPSO(_Device, _SampleCount, _RTFormat, _DSFormat);
    }

    void FAtmosphere::UpdatePerFrame(u32 _FrameSlot, const Vec3& _DirToSun, const Mat44& _InvViewProjNoTranslation) {
        FrameSlot = _FrameSlot;
        Vec3 d = _DirToSun.NormalizedSafe(Vec3{ 0.0f, 0.6f, 0.8f }.Normalized());
        CPUConstants.SunDir = { d.X, d.Y, d.Z, CPUConstants.SunDir.W }; // keep illuminance
        CPUConstants.InvViewProjNoTrans = _InvViewProjNoTranslation;
        // Copia o shadow inteiro p/ a regiao deste frame (inclui SunDisk dos setters).
        if (MappedBase) *Mapped() = CPUConstants;
    }

    void FAtmosphere::SetSunDiskHalfAngle(f32 _DegHalfAngle) {
        // So o shadow; aplicado na regiao do frame no proximo UpdatePerFrame.
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

    void FAtmosphere::Bake(ID3D12Device* _Device, FCommandQueue& _CmdQueue) {
        (void)_Device;
        if (MappedBase) *Mapped() = CPUConstants;

        _CmdQueue.ResetForRecording();
        auto* CL = _CmdQueue.List();

        ID3D12DescriptorHeap* Heaps[] = { SRVHeapPtr->Native() };
        CL->SetDescriptorHeaps(1, Heaps);

        const D3D12_GPU_VIRTUAL_ADDRESS CBAddr = this->CBAddr();

        // --- Transmittance LUT (no SRV input; t0 bound to a valid dummy slot) ---
        Transmittance.Transition(CL, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransmittancePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr);
        CL->SetComputeRootDescriptorTable(1, SRVHeapPtr->GpuHandle(Transmittance.SRVSlot));
        CL->SetComputeRootDescriptorTable(2, SRVHeapPtr->GpuHandle(Transmittance.UAVSlot));
        CL->Dispatch((kTransmittanceW + 7) / 8, (kTransmittanceH + 7) / 8, 1);
        Transmittance.Transition(CL, kReadState);

        // --- Multi-Scattering LUT (reads transmittance at t0) ---
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
