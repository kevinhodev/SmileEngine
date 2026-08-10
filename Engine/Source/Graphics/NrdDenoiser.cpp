#include "Smile/Graphics/NrdDenoiser.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"

#if SMILE_NRD_ENABLED
#include "NRD.h"
#include <malloc.h>
#include <string>
#include <cstring>
#include <cassert>

using Microsoft::WRL::ComPtr;

namespace {
    void* NRD_CALL NrdAlloc(void*, size_t size, size_t alignment)             { return _aligned_malloc(size, alignment ? alignment : 16); }
    void* NRD_CALL NrdRealloc(void*, void* mem, size_t size, size_t alignment){ return _aligned_realloc(mem, size, alignment ? alignment : 16); }
    void  NRD_CALL NrdFree(void*, void* mem)                                  { _aligned_free(mem); }

    DXGI_FORMAT ToDXGI(nrd::Format _Format) {
        using F = nrd::Format;
        switch (_Format) {
            case F::R8_UNORM:  return DXGI_FORMAT_R8_UNORM;
            case F::R8_SNORM:  return DXGI_FORMAT_R8_SNORM;
            case F::R8_UINT:   return DXGI_FORMAT_R8_UINT;
            case F::R8_SINT:   return DXGI_FORMAT_R8_SINT;
            case F::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
            case F::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
            case F::RG8_UINT:  return DXGI_FORMAT_R8G8_UINT;
            case F::RG8_SINT:  return DXGI_FORMAT_R8G8_SINT;
            case F::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case F::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
            case F::RGBA8_UINT:  return DXGI_FORMAT_R8G8B8A8_UINT;
            case F::RGBA8_SINT:  return DXGI_FORMAT_R8G8B8A8_SINT;
            case F::RGBA8_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case F::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
            case F::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
            case F::R16_UINT:  return DXGI_FORMAT_R16_UINT;
            case F::R16_SINT:  return DXGI_FORMAT_R16_SINT;
            case F::R16_SFLOAT:return DXGI_FORMAT_R16_FLOAT;
            case F::RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
            case F::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
            case F::RG16_UINT:  return DXGI_FORMAT_R16G16_UINT;
            case F::RG16_SINT:  return DXGI_FORMAT_R16G16_SINT;
            case F::RG16_SFLOAT:return DXGI_FORMAT_R16G16_FLOAT;
            case F::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
            case F::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
            case F::RGBA16_UINT:  return DXGI_FORMAT_R16G16B16A16_UINT;
            case F::RGBA16_SINT:  return DXGI_FORMAT_R16G16B16A16_SINT;
            case F::RGBA16_SFLOAT:return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case F::R32_UINT:  return DXGI_FORMAT_R32_UINT;
            case F::R32_SINT:  return DXGI_FORMAT_R32_SINT;
            case F::R32_SFLOAT:return DXGI_FORMAT_R32_FLOAT;
            case F::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
            case F::RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
            case F::RG32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
            case F::RGB32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
            case F::RGB32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
            case F::RGB32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
            case F::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
            case F::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
            case F::RGBA32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case F::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case F::R10_G10_B10_A2_UINT:  return DXGI_FORMAT_R10G10B10A2_UINT;
            case F::R11_G11_B10_UFLOAT:   return DXGI_FORMAT_R11G11B10_FLOAT;
            case F::R9_G9_B9_E5_UFLOAT:   return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
            default: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
    }

    DXGI_FORMAT IoFormat(Smile::FNrdDenoiser::EIo io) {
        using E = Smile::FNrdDenoiser;
        switch (io) {
            case E::IO_MV:                    return DXGI_FORMAT_R16G16_FLOAT;
            case E::IO_NORMAL_ROUGHNESS:      return DXGI_FORMAT_R10G10B10A2_UNORM;
            case E::IO_VIEWZ:                 return DXGI_FORMAT_R32_FLOAT;
            case E::IO_DIFF_RADIANCE_HITDIST: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case E::IO_SPEC_RADIANCE_HITDIST: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case E::IO_OUT_DIFF:              return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case E::IO_OUT_SPEC:              return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default:                          return DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
    }
}
#endif

namespace Smile {
#if SMILE_NRD_ENABLED
    namespace {
        ComPtr<ID3D12Resource> CreateTex(ID3D12Device* Dev, u32 W, u32 H, DXGI_FORMAT Fmt,
                                         const char* Label = "NRD · pools") {
            return GpuResources::CreateTex2D(Dev, W, H, Fmt,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI,
                                             nullptr, 1, 1, Label);
        }
    }
#endif

    void FNrdDenoiser::Initialize(ID3D12Device* _Device, ESignalProfile _Profile) {
#if SMILE_NRD_ENABLED
        if (Available) return;
        Device = _Device;
        SignalProfile = _Profile;
        const char* ProfileName = SignalProfile == ESignalProfile::Direct ? "Direta" : "Indireta";

        const nrd::LibraryDesc& LibraryDesc = *nrd::GetLibraryDesc();
        LogDebug("NRD " + std::string(ProfileName) + " v" + std::to_string(LibraryDesc.versionMajor) + "." + std::to_string(LibraryDesc.versionMinor) +
                "." + std::to_string(LibraryDesc.versionBuild) +
                " | Normal Encoding = " + std::to_string((int)LibraryDesc.normalEncoding) +
                " Roughness Encoding = " + std::to_string((int)LibraryDesc.roughnessEncoding));

        nrd::DenoiserDesc DenoiserDesc{};
        DenoiserDesc.identifier = 0;
        DenoiserDesc.denoiser   = nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
        nrd::InstanceCreationDesc InstanceCreationDesc{};
        InstanceCreationDesc.allocationCallbacks = { NrdAlloc, NrdRealloc, NrdFree, nullptr };
        InstanceCreationDesc.denoisers           = &DenoiserDesc;
        InstanceCreationDesc.denoisersNum        = 1;
        nrd::Result CreationResult = nrd::CreateInstance(InstanceCreationDesc, Instance);
        if (CreationResult != nrd::Result::SUCCESS || !Instance) {
            LogError("NRD CreateInstance Falhou (Resultado = " + std::to_string((int)CreationResult) + ")");
            Instance = nullptr;
            return;
        }

        const nrd::InstanceDesc& InstanceDesc = *nrd::GetInstanceDesc(*Instance);
        PerSetTex      = InstanceDesc.descriptorPoolDesc.perSetTexturesMaxNum;
        PerSetUav      = InstanceDesc.descriptorPoolDesc.perSetStorageTexturesMaxNum;
        SetsMax        = InstanceDesc.descriptorPoolDesc.setsMaxNum;
        TableStride    = PerSetTex + PerSetUav;
        ResourcesSpace = InstanceDesc.resourcesSpaceIndex;
        CbSpace        = InstanceDesc.constantBufferAndSamplersSpaceIndex;
        CbReg          = InstanceDesc.constantBufferRegisterIndex;
        SamplerBaseReg = InstanceDesc.samplersBaseRegisterIndex;
        LogDebug("NRD " + std::string(ProfileName) + " RELAX_DIFFUSE_SPECULAR: pipelines=" + std::to_string(InstanceDesc.pipelinesNum) +
                " perm=" + std::to_string(InstanceDesc.permanentPoolSize) +
                " trans=" + std::to_string(InstanceDesc.transientPoolSize) +
                " samplers=" + std::to_string(InstanceDesc.samplersNum) +
                " cbMax=" + std::to_string(InstanceDesc.constantBufferMaxDataSize));

        nrd::RelaxSettings RelaxSettings{};
        RelaxSettings.enableAntiFirefly = true;

        if (SignalProfile == ESignalProfile::Direct) {
            RelaxSettings.diffuseMaxAccumulatedFrameNum = 20;
            RelaxSettings.specularMaxAccumulatedFrameNum = 20;
            RelaxSettings.diffusePhiLuminance = 1.0f;
            RelaxSettings.spatialVarianceEstimationHistoryThreshold = 1;
            RelaxSettings.diffuseMaxFastAccumulatedFrameNum = 2;
            RelaxSettings.specularMaxFastAccumulatedFrameNum = 2;
            RelaxSettings.historyFixFrameNum = 1;
            RelaxSettings.fastHistoryClampingSigmaScale = 1.5f;
        }

        nrd::SetDenoiserSettings(*Instance, 0, &RelaxSettings);

        BuildRootSignature(_Device);
        BuildPipelines(_Device);
        CreateHeapsAndCB(_Device);
        Available = true;
#else
        (void)_Device; (void)_Profile;
        LogDebug("NRD Desabilitado (SMILE_NRD_ENABLED=0)");
#endif
    }

#if SMILE_NRD_ENABLED
    void FNrdDenoiser::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE DescriptorRanges[2]{};
        DescriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DescriptorRanges[0].NumDescriptors = PerSetTex;
        DescriptorRanges[0].BaseShaderRegister = 0;
        DescriptorRanges[0].RegisterSpace = ResourcesSpace;
        DescriptorRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        DescriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        DescriptorRanges[1].NumDescriptors = PerSetUav;
        DescriptorRanges[1].BaseShaderRegister = 0;
        DescriptorRanges[1].RegisterSpace = ResourcesSpace;
        DescriptorRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[2]{};
        RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = CbReg;
        RootParams[0].Descriptor.RegisterSpace  = CbSpace;
        RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[1].DescriptorTable.NumDescriptorRanges = 2;
        RootParams[1].DescriptorTable.pDescriptorRanges   = DescriptorRanges;
        RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC StaticSamplersDesc[2]{};
        for (u32 i = 0; i < 2; ++i) {
            StaticSamplersDesc[i].Filter = (i == 0) ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            StaticSamplersDesc[i].AddressU = StaticSamplersDesc[i].AddressV = StaticSamplersDesc[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            StaticSamplersDesc[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            StaticSamplersDesc[i].MaxLOD = D3D12_FLOAT32_MAX;
            StaticSamplersDesc[i].ShaderRegister = SamplerBaseReg + i;
            StaticSamplersDesc[i].RegisterSpace  = CbSpace;
            StaticSamplersDesc[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc{};
        RootSignatureDesc.NumParameters = 2;
        RootSignatureDesc.pParameters = RootParams;
        RootSignatureDesc.NumStaticSamplers = 2;
        RootSignatureDesc.pStaticSamplers = StaticSamplersDesc;
        RootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> Blob, Error;
        HRESULT hr = D3D12SerializeRootSignature(&RootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Error);
        if (FAILED(hr)) {
            if (Error) LogError(std::string("NRD Root Signature: ") + (const char*)Error->GetBufferPointer());
            SMILE_HR(hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                 IID_PPV_ARGS(&RootSignature)));
    }

    void FNrdDenoiser::BuildPipelines(ID3D12Device* _Device) {
        const nrd::InstanceDesc& InstanceDesc = *nrd::GetInstanceDesc(*Instance);
        Pipelines.resize(InstanceDesc.pipelinesNum);
        for (u32 i = 0; i < InstanceDesc.pipelinesNum; ++i) {
            const nrd::ComputeShaderDesc& ComputeShaderDesc = InstanceDesc.pipelines[i].computeShaderDXIL;
            assert(ComputeShaderDesc.bytecode && ComputeShaderDesc.size && "NRD Pipeline sem DXIL");
            D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc{};
            PSODesc.pRootSignature = RootSignature.Get();
            PSODesc.CS = { ComputeShaderDesc.bytecode, (SIZE_T)ComputeShaderDesc.size };
            SMILE_HR(_Device->CreateComputePipelineState(&PSODesc, IID_PPV_ARGS(&Pipelines[i])));
        }
    }

    void FNrdDenoiser::CreateHeapsAndCB(ID3D12Device* _Device) {
        const nrd::InstanceDesc& d = *nrd::GetInstanceDesc(*Instance);
        HandleSize = _Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        StagingCount = (d.permanentPoolSize + d.transientPoolSize + IO_COUNT) * 2;
        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = StagingCount;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        SMILE_HR(_Device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&StagingHeap)));

        const u32 frames = (u32)FCommandQueue::kFramesInFlight;
        TableRingCapacity = SetsMax * TableStride * frames;
        D3D12_DESCRIPTOR_HEAP_DESC th{};
        th.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        th.NumDescriptors = TableRingCapacity;
        th.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        SMILE_HR(_Device->CreateDescriptorHeap(&th, IID_PPV_ARGS(&TableHeap)));
        TableRingOffset = 0;

        ConstantBufferStride    = ((d.constantBufferMaxDataSize + 255u) / 256u) * 256u;
        ConstantBufferRingCount = SetsMax * frames;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cd{};
        cd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cd.Width = (UINT64)ConstantBufferStride * ConstantBufferRingCount;
        cd.Height = 1; cd.DepthOrArraySize = 1; cd.MipLevels = 1;
        cd.Format = DXGI_FORMAT_UNKNOWN; cd.SampleDesc = { 1, 0 };
        cd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ConstantBufferRing)));
        D3D12_RANGE nr{ 0, 0 };
        SMILE_HR(ConstantBufferRing->Map(0, &nr, reinterpret_cast<void**>(&ConstantBufferRingMapped)));
        ConstantBufferRingOffset = 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE FNrdDenoiser::StagingCpu(u32 Index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = StagingHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)Index * HandleSize;
        return h;
    }
#endif

    void FNrdDenoiser::SetupForResize(ID3D12Device* _Device, u32 _Width, u32 _Height) {
#if SMILE_NRD_ENABLED
        if (!Available) return;
        ReleaseResize();
        if (_Width == 0 || _Height == 0) return;
        RtWidth = _Width; RtHeight = _Height;
        const nrd::InstanceDesc& d = *nrd::GetInstanceDesc(*Instance);

        u32 staging = 0;
        auto Init = [&](FNRDTexture& T, ID3D12Resource* Res, DXGI_FORMAT Fmt) {
            T.Res = Res; T.State = D3D12_RESOURCE_STATE_COMMON;
            T.SRVStaging = staging++; T.UAVStaging = staging++;
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Format = Fmt; s.Texture2D.MipLevels = 1;
            _Device->CreateShaderResourceView(Res, &s, StagingCpu(T.SRVStaging));
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; u.Format = Fmt;
            _Device->CreateUnorderedAccessView(Res, nullptr, &u, StagingCpu(T.UAVStaging));
        };

        auto DivUp = [](u32 a, u32 b) { return b ? (a + b - 1) / b : a; };

        PermanentPool.resize(d.permanentPoolSize);
        for (u32 i = 0; i < d.permanentPoolSize; ++i) {
            const nrd::TextureDesc& td = d.permanentPool[i];
            DXGI_FORMAT fmt = ToDXGI(td.format);
            ComPtr<ID3D12Resource> res = CreateTex(_Device, DivUp(RtWidth, td.downsampleFactor),
                                                   DivUp(RtHeight, td.downsampleFactor), fmt,
                                                   PoolLabel());
            Init(PermanentPool[i], res.Get(), fmt);
        }
        TransientPool.resize(d.transientPoolSize);
        for (u32 i = 0; i < d.transientPoolSize; ++i) {
            const nrd::TextureDesc& td = d.transientPool[i];
            DXGI_FORMAT fmt = ToDXGI(td.format);
            ComPtr<ID3D12Resource> res = CreateTex(_Device, DivUp(RtWidth, td.downsampleFactor),
                                                   DivUp(RtHeight, td.downsampleFactor), fmt,
                                                   PoolLabel());
            Init(TransientPool[i], res.Get(), fmt);
        }
        for (u32 i = 0; i < IO_COUNT; ++i) {
            DXGI_FORMAT fmt = IoFormat((EIo)i);
            // IN/OUT, todas na resolucao CHEIA — e por isso que somam mais que os pools.
            ComPtr<ID3D12Resource> res = CreateTex(_Device, RtWidth, RtHeight, fmt,
                                                   SignalProfile == ESignalProfile::Direct
                                                       ? "NRD · IO (direta)" : "NRD · IO (indireta)");
            Init(Io[i], res.Get(), fmt);
        }
        assert(staging <= StagingCount && "NRD StagingHeap overflow");

        NeedsClear = true;
        Ready = true;
#else
        (void)_Device; (void)_Width; (void)_Height;
#endif
    }

#if SMILE_NRD_ENABLED
    void FNrdDenoiser::ReleaseResize() {
        PermanentPool.clear();
        TransientPool.clear();
        for (u32 i = 0; i < IO_COUNT; ++i) Io[i] = FNRDTexture{};
        Ready = false;
    }

    FNrdDenoiser::FNRDTexture* FNrdDenoiser::MapResource(u32 type, u32 indexInPool) {
        using RT = nrd::ResourceType;
        switch ((RT)type) {
            case RT::IN_MV:                    return &Io[IO_MV];
            case RT::IN_NORMAL_ROUGHNESS:      return &Io[IO_NORMAL_ROUGHNESS];
            case RT::IN_VIEWZ:                 return &Io[IO_VIEWZ];
            case RT::IN_DIFF_RADIANCE_HITDIST: return &Io[IO_DIFF_RADIANCE_HITDIST];
            case RT::IN_SPEC_RADIANCE_HITDIST: return &Io[IO_SPEC_RADIANCE_HITDIST];
            case RT::OUT_DIFF_RADIANCE_HITDIST:return &Io[IO_OUT_DIFF];
            case RT::OUT_SPEC_RADIANCE_HITDIST:return &Io[IO_OUT_SPEC];
            case RT::PERMANENT_POOL: return (indexInPool < PermanentPool.size())  ? &PermanentPool[indexInPool]  : nullptr;
            case RT::TRANSIENT_POOL: return (indexInPool < TransientPool.size()) ? &TransientPool[indexInPool] : nullptr;
            default: return nullptr;
        }
    }
#endif

    void FNrdDenoiser::SetFrame(const Mat44& _ViewToClip, const Mat44& _ViewToClipPrev,
                                const Mat44& _WorldToView, const Mat44& _WorldToViewPrev,
                                const Vec2& _Jitter, const Vec2& _JitterPrev, u32 _FrameIndex) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;
        nrd::CommonSettings cs{};
        std::memcpy(cs.viewToClipMatrix,      _ViewToClip.M,      sizeof(float) * 16);
        std::memcpy(cs.viewToClipMatrixPrev,  _ViewToClipPrev.M,  sizeof(float) * 16);
        std::memcpy(cs.worldToViewMatrix,     _WorldToView.M,     sizeof(float) * 16);
        std::memcpy(cs.worldToViewMatrixPrev, _WorldToViewPrev.M, sizeof(float) * 16);
        cs.motionVectorScale[0] = -1.0f; cs.motionVectorScale[1] = -1.0f; cs.motionVectorScale[2] = 0.0f;
        cs.cameraJitter[0] = _Jitter.X;      cs.cameraJitter[1] = _Jitter.Y;
        cs.cameraJitterPrev[0] = _JitterPrev.X; cs.cameraJitterPrev[1] = _JitterPrev.Y;
        cs.resourceSize[0] = cs.rectSize[0] = cs.resourceSizePrev[0] = cs.rectSizePrev[0] = (uint16_t)RtWidth;
        cs.resourceSize[1] = cs.rectSize[1] = cs.resourceSizePrev[1] = cs.rectSizePrev[1] = (uint16_t)RtHeight;
        cs.denoisingRange = 1.0e7f;
        cs.frameIndex = _FrameIndex;
        cs.accumulationMode = NeedsClear ? nrd::AccumulationMode::CLEAR_AND_RESTART
                                         : nrd::AccumulationMode::CONTINUE;
        cs.isMotionVectorInWorldSpace = false;
        nrd::SetCommonSettings(*Instance, cs);
#else
        (void)_ViewToClip; (void)_ViewToClipPrev; (void)_WorldToView; (void)_WorldToViewPrev;
        (void)_Jitter; (void)_JitterPrev; (void)_FrameIndex;
#endif
    }

#if SMILE_NRD_ENABLED
    void FNrdDenoiser::RecordDispatches(ID3D12GraphicsCommandList* _CommandList) {
        nrd::Identifier Identifier = 0;
        const nrd::DispatchDesc* DispatchDesc = nullptr;
        uint32_t Num = 0;
        nrd::Result Result = nrd::GetComputeDispatches(*Instance, &Identifier, 1, DispatchDesc, Num);
        if (Result != nrd::Result::SUCCESS || !DispatchDesc) {
            LogError("NRD GetComputeDispatches Falhou (Resultado = " + std::to_string((int)Result) + ")");
            return;
        }

        ID3D12DescriptorHeap* DescriptorHeaps[] = { TableHeap.Get() };
        _CommandList->SetDescriptorHeaps(1, DescriptorHeaps);
        _CommandList->SetComputeRootSignature(RootSignature.Get());

        const D3D12_GPU_DESCRIPTOR_HANDLE TableGPU0             = TableHeap->GetGPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE TableCpu0             = TableHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_VIRTUAL_ADDRESS   ConstantBufferBase    = ConstantBufferRing->GetGPUVirtualAddress();

        for (uint32_t DispatchIndex = 0; DispatchIndex < Num; ++DispatchIndex) {
            const nrd::DispatchDesc& dd = DispatchDesc[DispatchIndex];

            if (TableRingOffset + TableStride > TableRingCapacity) TableRingOffset = 0;
            if (ConstantBufferRingOffset + 1 > ConstantBufferRingCount) ConstantBufferRingOffset = 0;
            assert(dd.resourcesNum <= TableStride && "[NRD] Dispatch Excede a Tabela");

            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> SRVSource(PerSetTex, StagingCpu(Io[IO_OUT_DIFF].SRVStaging));
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> UAVSource(PerSetUav, StagingCpu(Io[IO_OUT_DIFF].UAVStaging));
            std::vector<D3D12_RESOURCE_BARRIER> ResourceBarriers;
            u32 SRVIndex = 0, UAVIndex = 0;

            for (uint32_t ResourceIndex = 0; ResourceIndex < dd.resourcesNum; ++ResourceIndex) {
                const nrd::ResourceDesc& ResourceDesc = dd.resources[ResourceIndex];
                FNRDTexture* NRDTexture = MapResource((u32)ResourceDesc.type, ResourceDesc.indexInPool);
                if (!NRDTexture || !NRDTexture->Res) { LogError("[NRD] Recurso Não Mapeado"); continue; }
                if (ResourceDesc.descriptorType == nrd::DescriptorType::TEXTURE) {
                    if (SRVIndex < PerSetTex) SRVSource[SRVIndex++] = StagingCpu(NRDTexture->SRVStaging);
                    if (NRDTexture->State != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
                        D3D12_RESOURCE_BARRIER ResourceBarrier{};
                        ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        ResourceBarrier.Transition.pResource = NRDTexture->Res.Get();
                        ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        ResourceBarrier.Transition.StateBefore = NRDTexture->State; ResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        ResourceBarriers.push_back(ResourceBarrier); NRDTexture->State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    }
                } else {
                    if (UAVIndex < PerSetUav) UAVSource[UAVIndex++] = StagingCpu(NRDTexture->UAVStaging);
                    if (NRDTexture->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                        D3D12_RESOURCE_BARRIER ResourceBarrier{};
                        ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        ResourceBarrier.Transition.pResource = NRDTexture->Res.Get();
                        ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        ResourceBarrier.Transition.StateBefore = NRDTexture->State;
                        ResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        ResourceBarriers.push_back(ResourceBarrier);
                        NRDTexture->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    } else {
                        D3D12_RESOURCE_BARRIER ResourceBarrier{};
                        ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        ResourceBarrier.UAV.pResource = NRDTexture->Res.Get();
                        ResourceBarriers.push_back(ResourceBarrier);
                    }
                }
            }
            if (!ResourceBarriers.empty()) _CommandList->ResourceBarrier((UINT)ResourceBarriers.size(), ResourceBarriers.data());

            D3D12_CPU_DESCRIPTOR_HANDLE dstSrv = TableCpu0;
            dstSrv.ptr += (SIZE_T)TableRingOffset * HandleSize;
            D3D12_CPU_DESCRIPTOR_HANDLE dstUav = dstSrv;
            dstUav.ptr += (SIZE_T)PerSetTex * HandleSize;
            std::vector<UINT> onesS(PerSetTex, 1u), onesU(PerSetUav, 1u);
            Device->CopyDescriptors(1, &dstSrv, &PerSetTex, PerSetTex, SRVSource.data(), onesS.data(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            Device->CopyDescriptors(1, &dstUav, &PerSetUav, PerSetUav, UAVSource.data(), onesU.data(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_GPU_DESCRIPTOR_HANDLE TableGPU = TableGPU0;
            TableGPU.ptr += (UINT64)TableRingOffset * HandleSize;

            u8* cbDst = ConstantBufferRingMapped + (size_t)ConstantBufferRingOffset * ConstantBufferStride;
            std::memcpy(cbDst, dd.constantBufferData, dd.constantBufferDataSize);
            D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = ConstantBufferBase + (UINT64)ConstantBufferRingOffset * ConstantBufferStride;

            _CommandList->SetPipelineState(Pipelines[dd.pipelineIndex].Get());
            _CommandList->SetComputeRootConstantBufferView(0, ConstantBufferAddress);
            _CommandList->SetComputeRootDescriptorTable(1, TableGPU);
            _CommandList->Dispatch(dd.gridWidth, dd.gridHeight, 1);

            TableRingOffset += TableStride;
            ConstantBufferRingOffset += 1;
        }

        NeedsClear = false;
    }
#endif

    void FNrdDenoiser::Denoise(ID3D12GraphicsCommandList* _CommandList) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;
        RecordDispatches(_CommandList);
#else
        (void)_CommandList;
#endif
    }

    void FNrdDenoiser::TransitionInputsToWrite(ID3D12GraphicsCommandList* _CommandList) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;
        const EIo Inputs[5] = { IO_MV, IO_NORMAL_ROUGHNESS, IO_VIEWZ,
                             IO_DIFF_RADIANCE_HITDIST, IO_SPEC_RADIANCE_HITDIST };
        std::vector<D3D12_RESOURCE_BARRIER> ResourceBarriers;
        for (EIo Input : Inputs) {
            FNRDTexture& NRDTexture = Io[Input];
            if (!NRDTexture.Res || NRDTexture.State == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) continue;
            D3D12_RESOURCE_BARRIER ResourceBarrier{};
            ResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ResourceBarrier.Transition.pResource = NRDTexture.Res.Get();
            ResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ResourceBarrier.Transition.StateBefore = NRDTexture.State;
            ResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            ResourceBarriers.push_back(ResourceBarrier);
            NRDTexture.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (!ResourceBarriers.empty()) _CommandList->ResourceBarrier((UINT)ResourceBarriers.size(), ResourceBarriers.data());
#else
        (void)_CommandList;
#endif
    }

    void FNrdDenoiser::TransitionOutputToRead(ID3D12GraphicsCommandList* _CommandList) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;

        constexpr D3D12_RESOURCE_STATES ShaderRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const EIo Outputs[2] = { IO_OUT_DIFF, IO_OUT_SPEC };
        std::vector<D3D12_RESOURCE_BARRIER> bs;
        for (EIo Output : Outputs) {
            FNRDTexture& NRDTexture = Io[Output];
            if (!NRDTexture.Res || NRDTexture.State == ShaderRead) continue;
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = NRDTexture.Res.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = NRDTexture.State;
            b.Transition.StateAfter = ShaderRead;
            bs.push_back(b);
            NRDTexture.State = ShaderRead;
        }
        if (!bs.empty()) _CommandList->ResourceBarrier((UINT)bs.size(), bs.data());
#else
        (void)_CommandList;
#endif
    }

    ID3D12Resource* FNrdDenoiser::IoResource(EIo _Which) const {
#if SMILE_NRD_ENABLED
        return (_Which < IO_COUNT) ? Io[_Which].Res.Get() : nullptr;
#else
        (void)_Which;
        return nullptr;
#endif
    }

    void FNrdDenoiser::Shutdown() {
#if SMILE_NRD_ENABLED
        if (ConstantBufferRing && ConstantBufferRingMapped) { ConstantBufferRing->Unmap(0, nullptr); ConstantBufferRingMapped = nullptr; }
        ReleaseResize();
        Pipelines.clear();
        if (Instance) { nrd::DestroyInstance(*Instance); Instance = nullptr; }
#endif
        Available = false;
        Ready = false;
    }
}
