#include "Smile/Graphics/SunShafts.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Smile {
    void FSunShafts::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                u32 _RenderWidth, u32 _RenderHeight) {
        if (Initialized) return;
        NativeDevice = _Device;
        BuildRootSignature(_Device);
        BuildPSOs(_Device);
        CreateConstantBuffer(_Device);
        Resize(_Device, _SRVHeap, _RenderWidth, _RenderHeight);
        Initialized = true;
        LogInfo("Sun shafts F1: froxels temporais + CSM inicializados");
    }

    void FSunShafts::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 2;
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE UAVRange{};
        UAVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        UAVRange.NumDescriptors                    = 1;
        UAVRange.BaseShaderRegister                = 0;
        UAVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER Params[4]{};
        Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister = 0;
        Params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        Params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[1].Descriptor.ShaderRegister = 1;
        Params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        Params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[2].DescriptorTable.NumDescriptorRanges = 1;
        Params[2].DescriptorTable.pDescriptorRanges   = &SRVRange;
        Params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        Params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[3].DescriptorTable.NumDescriptorRanges = 1;
        Params[3].DescriptorTable.pDescriptorRanges   = &UAVRange;
        Params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        Samplers[1]                  = Samplers[0];
        Samplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        Samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        Samplers[1].ShaderRegister   = 1;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(Params);
        Desc.pParameters       = Params;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob;
        Microsoft::WRL::ComPtr<ID3DBlob> Errors;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &Blob, &Errors);
        if (FAILED(Hr)) {
            if (Errors)
                LogError(std::string("Sun shafts root sig: ") +
                         static_cast<const char*>(Errors->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                               IID_PPV_ARGS(&RootSignature)));
    }

    void FSunShafts::BuildPSOs(ID3D12Device* _Device) {
        auto Build = [&](const char* Name, Microsoft::WRL::ComPtr<ID3D12PipelineState>& Out) {
            auto CS = LoadShaderBytecode(Name);
            D3D12_COMPUTE_PIPELINE_STATE_DESC Desc{};
            Desc.pRootSignature = RootSignature.Get();
            Desc.CS = { CS.data(), CS.size() };
            SMILE_HR(_Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&Out)));
        };
        Build("SunShaftsScatter.cs_6_0.cso", ScatterPSO);
        Build("SunShaftsIntegrate.cs_6_0.cso", IntegratePSO);
    }

    void FSunShafts::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                                sizeof(SunShaftConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(_Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ConstantBuffer)));

        D3D12_RANGE NoRead{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, &Ptr));
        MappedBase = reinterpret_cast<u8*>(Ptr);
        std::memset(MappedBase, 0, static_cast<size_t>(Desc.Width));
    }

    void FSunShafts::Resize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                            u32 _RenderWidth, u32 _RenderHeight) {
        GridWidth  = std::max(1u, (_RenderWidth  + kGridPixelSize - 1) / kGridPixelSize);
        GridHeight = std::max(1u, (_RenderHeight + kGridPixelSize - 1) / kGridPixelSize);

        auto ReleaseVolume = [&](FVolumeTexture& Volume) {
            if (!Volume.IsValid()) return;
            _SRVHeap.Free(Volume.SRVSlot());
            for (u32 Mip = 0; Mip < Volume.MipCount(); ++Mip)
                _SRVHeap.Free(Volume.UAVSlot(Mip));
            Volume = FVolumeTexture{};
        };
        ReleaseVolume(Scattering[0]);
        ReleaseVolume(Scattering[1]);
        ReleaseVolume(Integrated);

        for (u32 i = 0; i < 2; ++i)
            Scattering[i].Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 GridWidth, GridHeight, kGridSizeZ, 1, true);
        Integrated.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          GridWidth, GridHeight, kGridSizeZ, 1, true);

        for (u32 i = 0; i < 2; ++i) {
            if (ScatterTable[i] == kInvalidSlot)   ScatterTable[i]   = _SRVHeap.Allocate(2);
            if (IntegrateTable[i] == kInvalidSlot) IntegrateTable[i] = _SRVHeap.Allocate(2);
        }
        RefreshDescriptorTables(_SRVHeap);
        HistoryValid = false;
        LastCameraValid = false;
        FrameCounter = 0;
    }

    void FSunShafts::RefreshDescriptorTables(FTextureSRVHeap& _SRVHeap) {
        for (u32 i = 0; i < 2; ++i) {
            NativeDevice->CopyDescriptorsSimple(
                1, _SRVHeap.CpuHandle(ScatterTable[i] + 1),
                _SRVHeap.CpuHandleStaging(Scattering[1 - i].SRVSlot()),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            NativeDevice->CopyDescriptorsSimple(
                1, _SRVHeap.CpuHandle(IntegrateTable[i]),
                _SRVHeap.CpuHandleStaging(Scattering[i].SRVSlot()),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            NativeDevice->CopyDescriptorsSimple(
                1, _SRVHeap.CpuHandle(IntegrateTable[i] + 1),
                _SRVHeap.CpuHandleStaging(Scattering[i].SRVSlot()),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    void FSunShafts::UpdatePerFrame(u32 _FrameSlot, const Mat44& _InvViewProjUnjittered,
                                    const Mat44& _PrevViewProjUnjittered,
                                    const Vec3& _CameraWorldPos, const Vec3& _DirToLight,
                                    const Vec3& _LightColor, f32 _LightIntensity,
                                    const Vec4& _FogLayer1, const Vec4& _FogLayer2,
                                    f32 _FarZ, const Vec3& _Jitter, bool _Active) {
        FrameSlot = _FrameSlot;
        Active = _Active;
        if (!Active || !MappedBase) {
            HistoryValid = false;
            LastCameraValid = false;
            return;
        }

        const f32 Cut = std::max(25.0f, MaxDistance * 0.10f);
        if (LastCameraValid) {
            const f32 DX = _CameraWorldPos.X - LastCameraPosition.X;
            const f32 DY = _CameraWorldPos.Y - LastCameraPosition.Y;
            const f32 DZ = _CameraWorldPos.Z - LastCameraPosition.Z;
            if (DX * DX + DY * DY + DZ * DZ > Cut * Cut)
                HistoryValid = false;
        }

        SunShaftConstants C{};
        C.InvViewProj             = _InvViewProjUnjittered;
        C.PrevViewProj            = _PrevViewProjUnjittered;
        C.CameraWorldPos          = { _CameraWorldPos.X, _CameraWorldPos.Y, _CameraWorldPos.Z,
                                      std::clamp(HistoryWeight, 0.0f, 0.98f) };
        const Vec3 LightN = _DirToLight.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
        C.SunDirectionIntensity   = { LightN.X, LightN.Y, LightN.Z,
                                      std::max(_LightIntensity * Intensity, 0.0f) };
        C.SunColorPhase           = { _LightColor.X, _LightColor.Y, _LightColor.Z,
                                      std::clamp(Anisotropy, -0.9f, 0.9f) };
        C.FogLayer1               = _FogLayer1;
        C.FogLayer2               = { _FogLayer2.X, _FogLayer2.Y, _FogLayer2.Z, _FarZ };
        C.GridParams              = { static_cast<f32>(GridWidth), static_cast<f32>(GridHeight),
                                      static_cast<f32>(kGridSizeZ), std::max(MaxDistance, 1.0f) };
        C.TemporalParams          = { _Jitter.X, _Jitter.Y, _Jitter.Z,
                                      HistoryValid ? 1.0f : 0.0f };

        std::memcpy(MappedBase + static_cast<size_t>(FrameSlot) * sizeof(SunShaftConstants),
                    &C, sizeof(C));
        LastCameraPosition = _CameraWorldPos;
        LastCameraValid = true;
    }

    D3D12_GPU_VIRTUAL_ADDRESS FSunShafts::CBAddress() const {
        return ConstantBuffer->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(SunShaftConstants);
    }

    void FSunShafts::Record(ID3D12GraphicsCommandList* _CommandList,
                            FTextureSRVHeap& _SRVHeap,
                            D3D12_GPU_VIRTUAL_ADDRESS _CSMConstants,
                            u32 _ShadowMapSRVSlot) {
        if (!HasWork()) return;

        const u32 Curr = static_cast<u32>(FrameCounter & 1u);
        const u32 Prev = 1u - Curr;

        NativeDevice->CopyDescriptorsSimple(
            1, _SRVHeap.CpuHandle(ScatterTable[Curr]),
            _SRVHeap.CpuHandleStaging(_ShadowMapSRVSlot),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        Scattering[Prev].Transition(_CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Scattering[Curr].Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Integrated.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        _CommandList->SetComputeRootSignature(RootSignature.Get());
        _CommandList->SetComputeRootConstantBufferView(0, CBAddress());
        _CommandList->SetComputeRootConstantBufferView(1, _CSMConstants);

        _CommandList->SetPipelineState(ScatterPSO.Get());
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ScatterTable[Curr]));
        _CommandList->SetComputeRootDescriptorTable(3, _SRVHeap.GpuHandle(Scattering[Curr].UAVSlot(0)));
        _CommandList->Dispatch((GridWidth + 3) / 4, (GridHeight + 3) / 4,
                               (kGridSizeZ + 3) / 4);

        Scattering[Curr].Transition(_CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        _CommandList->SetPipelineState(IntegratePSO.Get());
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(IntegrateTable[Curr]));
        _CommandList->SetComputeRootDescriptorTable(3, _SRVHeap.GpuHandle(Integrated.UAVSlot(0)));
        _CommandList->Dispatch((GridWidth + 7) / 8, (GridHeight + 7) / 8, 1);

        Integrated.Transition(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        HistoryValid = true;
        ++FrameCounter;
    }
}
