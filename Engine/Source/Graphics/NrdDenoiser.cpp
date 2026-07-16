#include "Smile/Graphics/NrdDenoiser.h"
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
    void* NRD_CALL NrdAlloc(void*, size_t size, size_t alignment)            { return _aligned_malloc(size, alignment ? alignment : 16); }
    void* NRD_CALL NrdRealloc(void*, void* mem, size_t size, size_t alignment){ return _aligned_realloc(mem, size, alignment ? alignment : 16); }
    void  NRD_CALL NrdFree(void*, void* mem)                                 { _aligned_free(mem); }

    DXGI_FORMAT ToDXGI(nrd::Format f) {
        using F = nrd::Format;
        switch (f) {
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
        ComPtr<ID3D12Resource> CreateTex(ID3D12Device* Dev, u32 W, u32 H, DXGI_FORMAT Fmt) {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC D{};
            D.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            D.Width = W; D.Height = H; D.DepthOrArraySize = 1; D.MipLevels = 1;
            D.Format = Fmt; D.SampleDesc = { 1, 0 };
            D.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            D.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> T;
            SMILE_HR(Dev->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &D,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&T)));
            VramTracker::Register(T.Get(), EVramCategory::GI);
            return T;
        }
    }
#endif

    void FNrdDenoiser::Initialize(ID3D12Device* _Device) {
#if SMILE_NRD_ENABLED
        if (Available) return;
        Dev = _Device;

        const nrd::LibraryDesc& lib = *nrd::GetLibraryDesc();
        LogInfo("NRD v" + std::to_string(lib.versionMajor) + "." + std::to_string(lib.versionMinor) +
                "." + std::to_string(lib.versionBuild) +
                " | normalEnc=" + std::to_string((int)lib.normalEncoding) +
                " roughEnc=" + std::to_string((int)lib.roughnessEncoding));

        nrd::DenoiserDesc denoiser{};
        denoiser.identifier = 0;
        denoiser.denoiser   = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
        nrd::InstanceCreationDesc icd{};
        icd.allocationCallbacks = { NrdAlloc, NrdRealloc, NrdFree, nullptr };
        icd.denoisers           = &denoiser;
        icd.denoisersNum        = 1;
        nrd::Result r = nrd::CreateInstance(icd, Instance);
        if (r != nrd::Result::SUCCESS || !Instance) {
            LogError("NRD CreateInstance falhou (Result=" + std::to_string((int)r) + ")");
            Instance = nullptr; return;
        }

        const nrd::InstanceDesc& d = *nrd::GetInstanceDesc(*Instance);
        PerSetTex      = d.descriptorPoolDesc.perSetTexturesMaxNum;
        PerSetUav      = d.descriptorPoolDesc.perSetStorageTexturesMaxNum;
        SetsMax        = d.descriptorPoolDesc.setsMaxNum;
        TableStride    = PerSetTex + PerSetUav;
        ResourcesSpace = d.resourcesSpaceIndex;
        CbSpace        = d.constantBufferAndSamplersSpaceIndex;
        CbReg          = d.constantBufferRegisterIndex;
        SamplerBaseReg = d.samplersBaseRegisterIndex;
        LogInfo("NRD REBLUR_DIFFUSE_SPECULAR: pipelines=" + std::to_string(d.pipelinesNum) +
                " perm=" + std::to_string(d.permanentPoolSize) +
                " trans=" + std::to_string(d.transientPoolSize) +
                " samplers=" + std::to_string(d.samplersNum) +
                " cbMax=" + std::to_string(d.constantBufferMaxDataSize));

        nrd::ReblurSettings reblur{};
        // Prepass difuso no default do NRD (config estavel do bisect 2026-07-12; o override 0
        // do fix 6 saiu junto com o pacote).
        reblur.enableAntiFirefly = true;
        nrd::SetDenoiserSettings(*Instance, 0, &reblur);

        BuildRootSignature(_Device);
        BuildPipelines(_Device);
        CreateHeapsAndCB(_Device);
        Available = true;
#else
        (void)_Device;
        LogWarning("NRD desabilitado (SMILE_NRD_ENABLED=0)");
#endif
    }

#if SMILE_NRD_ENABLED
    void FNrdDenoiser::BuildRootSignature(ID3D12Device* _Device) {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = PerSetTex;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = ResourcesSpace;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = PerSetUav;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = ResourcesSpace;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = CbReg;
        params[0].Descriptor.RegisterSpace  = CbSpace;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges   = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp[2]{};
        for (u32 i = 0; i < 2; ++i) {
            samp[i].Filter = (i == 0) ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            samp[i].MaxLOD = D3D12_FLOAT32_MAX;
            samp[i].ShaderRegister = SamplerBaseReg + i;
            samp[i].RegisterSpace  = CbSpace;
            samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2; rs.pParameters = params;
        rs.NumStaticSamplers = 2; rs.pStaticSamplers = samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) {
            if (err) LogError(std::string("NRD root sig: ") + (const char*)err->GetBufferPointer());
            SMILE_HR(hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                 IID_PPV_ARGS(&RootSig)));
    }

    void FNrdDenoiser::BuildPipelines(ID3D12Device* _Device) {
        const nrd::InstanceDesc& d = *nrd::GetInstanceDesc(*Instance);
        Pipelines.resize(d.pipelinesNum);
        for (u32 i = 0; i < d.pipelinesNum; ++i) {
            const nrd::ComputeShaderDesc& cs = d.pipelines[i].computeShaderDXIL;
            assert(cs.bytecode && cs.size && "NRD pipeline sem DXIL");
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
            ps.pRootSignature = RootSig.Get();
            ps.CS = { cs.bytecode, (SIZE_T)cs.size };
            SMILE_HR(_Device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&Pipelines[i])));
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

        CbStride    = ((d.constantBufferMaxDataSize + 255u) / 256u) * 256u;
        CbRingCount = SetsMax * frames;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cd{};
        cd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cd.Width = (UINT64)CbStride * CbRingCount;
        cd.Height = 1; cd.DepthOrArraySize = 1; cd.MipLevels = 1;
        cd.Format = DXGI_FORMAT_UNKNOWN; cd.SampleDesc = { 1, 0 };
        cd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CbRing)));
        D3D12_RANGE nr{ 0, 0 };
        SMILE_HR(CbRing->Map(0, &nr, reinterpret_cast<void**>(&CbRingMapped)));
        CbRingOffset = 0;
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
        auto Init = [&](FNrdTexture& T, ID3D12Resource* Res, DXGI_FORMAT Fmt) {
            T.Res = Res; T.State = D3D12_RESOURCE_STATE_COMMON;
            T.SrvStaging = staging++; T.UavStaging = staging++;
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Format = Fmt; s.Texture2D.MipLevels = 1;
            _Device->CreateShaderResourceView(Res, &s, StagingCpu(T.SrvStaging));
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; u.Format = Fmt;
            _Device->CreateUnorderedAccessView(Res, nullptr, &u, StagingCpu(T.UavStaging));
        };

        auto DivUp = [](u32 a, u32 b) { return b ? (a + b - 1) / b : a; };

        PermPool.resize(d.permanentPoolSize);
        for (u32 i = 0; i < d.permanentPoolSize; ++i) {
            const nrd::TextureDesc& td = d.permanentPool[i];
            DXGI_FORMAT fmt = ToDXGI(td.format);
            ComPtr<ID3D12Resource> res = CreateTex(_Device, DivUp(RtWidth, td.downsampleFactor),
                                                   DivUp(RtHeight, td.downsampleFactor), fmt);
            Init(PermPool[i], res.Get(), fmt); 
        }
        TransPool.resize(d.transientPoolSize);
        for (u32 i = 0; i < d.transientPoolSize; ++i) {
            const nrd::TextureDesc& td = d.transientPool[i];
            DXGI_FORMAT fmt = ToDXGI(td.format);
            ComPtr<ID3D12Resource> res = CreateTex(_Device, DivUp(RtWidth, td.downsampleFactor),
                                                   DivUp(RtHeight, td.downsampleFactor), fmt);
            Init(TransPool[i], res.Get(), fmt);
        }
        for (u32 i = 0; i < IO_COUNT; ++i) {
            DXGI_FORMAT fmt = IoFormat((EIo)i);
            ComPtr<ID3D12Resource> res = CreateTex(_Device, RtWidth, RtHeight, fmt);
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
        PermPool.clear(); TransPool.clear(); 
        for (u32 i = 0; i < IO_COUNT; ++i) Io[i] = FNrdTexture{};
        Ready = false;
    }

    FNrdDenoiser::FNrdTexture* FNrdDenoiser::MapResource(u32 type, u32 indexInPool) {
        using RT = nrd::ResourceType;
        switch ((RT)type) {
            case RT::IN_MV:                    return &Io[IO_MV];
            case RT::IN_NORMAL_ROUGHNESS:      return &Io[IO_NORMAL_ROUGHNESS];
            case RT::IN_VIEWZ:                 return &Io[IO_VIEWZ];
            case RT::IN_DIFF_RADIANCE_HITDIST: return &Io[IO_DIFF_RADIANCE_HITDIST];
            case RT::IN_SPEC_RADIANCE_HITDIST: return &Io[IO_SPEC_RADIANCE_HITDIST];
            case RT::OUT_DIFF_RADIANCE_HITDIST:return &Io[IO_OUT_DIFF];
            case RT::OUT_SPEC_RADIANCE_HITDIST:return &Io[IO_OUT_SPEC];
            case RT::PERMANENT_POOL: return (indexInPool < PermPool.size())  ? &PermPool[indexInPool]  : nullptr;
            case RT::TRANSIENT_POOL: return (indexInPool < TransPool.size()) ? &TransPool[indexInPool] : nullptr;
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
    void FNrdDenoiser::RecordDispatches(ID3D12GraphicsCommandList* _CL) {
        nrd::Identifier id = 0;
        const nrd::DispatchDesc* dispatches = nullptr;
        uint32_t num = 0;
        nrd::Result r = nrd::GetComputeDispatches(*Instance, &id, 1, dispatches, num);
        if (r != nrd::Result::SUCCESS || !dispatches) {
            LogError("NRD GetComputeDispatches falhou (Result=" + std::to_string((int)r) + ")");
            return;
        }

        ID3D12DescriptorHeap* heaps[] = { TableHeap.Get() };
        _CL->SetDescriptorHeaps(1, heaps);
        _CL->SetComputeRootSignature(RootSig.Get());

        const D3D12_GPU_DESCRIPTOR_HANDLE tableGpu0 = TableHeap->GetGPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE tableCpu0 = TableHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_VIRTUAL_ADDRESS  cbBase     = CbRing->GetGPUVirtualAddress();

        for (uint32_t di = 0; di < num; ++di) {
            const nrd::DispatchDesc& dd = dispatches[di];

            if (TableRingOffset + TableStride > TableRingCapacity) TableRingOffset = 0;
            if (CbRingOffset + 1 > CbRingCount) CbRingOffset = 0;
            assert(dd.resourcesNum <= TableStride && "NRD dispatch excede a tabela");

            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvSrc(PerSetTex, StagingCpu(Io[IO_OUT_DIFF].SrvStaging));
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> uavSrc(PerSetUav, StagingCpu(Io[IO_OUT_DIFF].UavStaging));
            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            u32 srvIdx = 0, uavIdx = 0;
            for (uint32_t ri = 0; ri < dd.resourcesNum; ++ri) {
                const nrd::ResourceDesc& rd = dd.resources[ri];
                FNrdTexture* t = MapResource((u32)rd.type, rd.indexInPool);
                if (!t || !t->Res) { LogError("NRD recurso nao mapeado"); continue; }
                if (rd.descriptorType == nrd::DescriptorType::TEXTURE) {
                    if (srvIdx < PerSetTex) srvSrc[srvIdx++] = StagingCpu(t->SrvStaging);
                    if (t->State != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
                        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = t->Res.Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = t->State; b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        barriers.push_back(b); t->State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    }
                } else {
                    if (uavIdx < PerSetUav) uavSrc[uavIdx++] = StagingCpu(t->UavStaging);
                    if (t->State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = t->Res.Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = t->State; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        barriers.push_back(b); t->State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    } else {
                        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = t->Res.Get(); barriers.push_back(b); 
                    }
                }
            }
            if (!barriers.empty()) _CL->ResourceBarrier((UINT)barriers.size(), barriers.data());

            D3D12_CPU_DESCRIPTOR_HANDLE dstSrv = tableCpu0; dstSrv.ptr += (SIZE_T)TableRingOffset * HandleSize;
            D3D12_CPU_DESCRIPTOR_HANDLE dstUav = dstSrv;    dstUav.ptr += (SIZE_T)PerSetTex * HandleSize;
            std::vector<UINT> onesS(PerSetTex, 1u), onesU(PerSetUav, 1u);
            Dev->CopyDescriptors(1, &dstSrv, &PerSetTex, PerSetTex, srvSrc.data(), onesS.data(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            Dev->CopyDescriptors(1, &dstUav, &PerSetUav, PerSetUav, uavSrc.data(), onesU.data(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = tableGpu0; tableGpu.ptr += (UINT64)TableRingOffset * HandleSize;

            u8* cbDst = CbRingMapped + (size_t)CbRingOffset * CbStride;
            std::memcpy(cbDst, dd.constantBufferData, dd.constantBufferDataSize);
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr = cbBase + (UINT64)CbRingOffset * CbStride;

            _CL->SetPipelineState(Pipelines[dd.pipelineIndex].Get());
            _CL->SetComputeRootConstantBufferView(0, cbAddr);
            _CL->SetComputeRootDescriptorTable(1, tableGpu);
            _CL->Dispatch(dd.gridWidth, dd.gridHeight, 1);

            TableRingOffset += TableStride;
            CbRingOffset += 1;
        }

        if (!SelfTestLogged) {
            LogInfo("NRD driver: " + std::to_string(num) + " dispatches executados (bring-up OK)");
            SelfTestLogged = true;
        }
        NeedsClear = false;
    }
#endif

    void FNrdDenoiser::RunSelfTest(ID3D12GraphicsCommandList* _CL) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;
        RecordDispatches(_CL);
#else
        (void)_CL;
#endif
    }

    void FNrdDenoiser::Denoise(ID3D12GraphicsCommandList* _CL) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;
        RecordDispatches(_CL);
#else
        (void)_CL;
#endif
    }

    void FNrdDenoiser::TransitionInputsToWrite(ID3D12GraphicsCommandList* _CL) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;
        const EIo ins[5] = { IO_MV, IO_NORMAL_ROUGHNESS, IO_VIEWZ,
                             IO_DIFF_RADIANCE_HITDIST, IO_SPEC_RADIANCE_HITDIST };
        std::vector<D3D12_RESOURCE_BARRIER> bs;
        for (EIo e : ins) {
            FNrdTexture& t = Io[e];
            if (!t.Res || t.State == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) continue;
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = t.Res.Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = t.State; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            bs.push_back(b); t.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (!bs.empty()) _CL->ResourceBarrier((UINT)bs.size(), bs.data());
#else
        (void)_CL;
#endif
    }

    void FNrdDenoiser::TransitionOutputToRead(ID3D12GraphicsCommandList* _CL) {
#if SMILE_NRD_ENABLED
        if (!Ready) return;

        const EIo outs[2] = { IO_OUT_DIFF, IO_OUT_SPEC };
        std::vector<D3D12_RESOURCE_BARRIER> bs;
        for (EIo e : outs) {
            FNrdTexture& t = Io[e];
            if (!t.Res || t.State == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) continue;
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = t.Res.Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = t.State; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            bs.push_back(b); t.State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        if (!bs.empty()) _CL->ResourceBarrier((UINT)bs.size(), bs.data());
#else
        (void)_CL;
#endif
    }

    ID3D12Resource* FNrdDenoiser::IoResource(EIo Which) const {
#if SMILE_NRD_ENABLED
        return (Which < IO_COUNT) ? Io[Which].Res.Get() : nullptr;
#else
        (void)Which; return nullptr;
#endif
    }

    void FNrdDenoiser::Shutdown() {
#if SMILE_NRD_ENABLED
        if (CbRing && CbRingMapped) { CbRing->Unmap(0, nullptr); CbRingMapped = nullptr; }
        ReleaseResize();
        Pipelines.clear();
        if (Instance) { nrd::DestroyInstance(*Instance); Instance = nullptr; }
#endif
        Available = false;
        Ready = false;
    }
}
