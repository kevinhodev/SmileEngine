#include "Smile/Graphics/VolumetricFog.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cmath>
#include <cstring>

namespace Smile {

    void FVolumetricFogPass::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (Initialized) return;

        VBufferA.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                        kGridW, kGridH, kGridZ, 1, true);
        LightScatteringPP[0].Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                    kGridW, kGridH, kGridZ, 1, true);
        LightScatteringPP[1].Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                    kGridW, kGridH, kGridZ, 1, true);
        Integrated.Create(_Device, _SRVHeap, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          kGridW, kGridH, kGridZ, 1, true);

        SetupPSO.Initialize(_Device, "VolumetricFogSetup.cs_6_0.cso", 1, 1);
        IntegratePSO.Initialize(_Device, "VolumetricFogIntegrate.cs_6_0.cso", 1, 1);
        ConsDepthPSO.Initialize(_Device, "VolumetricFogConsDepth.cs_6_0.cso", 1, 1);

        // Conservative depth 160x90 R16F ping-pong (cur = min-Z deste frame; prev
        // alimenta o fixup da historia contra desoclusao com fog fantasma).
        for (u32 i = 0; i < 2; ++i) {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = kGridW;
            Desc.Height           = kGridH;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_R16_FLOAT;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            SMILE_HR(_Device->CreateCommittedResource(
                &Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(&ConsDepthTex[i])));
            VramTracker::Register(ConsDepthTex[i].Get(), EVramCategory::Sky);

            ConsDepthSRV[i] = _SRVHeap.Allocate(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
            SRVDesc.Format                    = DXGI_FORMAT_R16_FLOAT;
            SRVDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            SRVDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SRVDesc.Texture2D.MipLevels       = 1;
            _SRVHeap.CreateSRV(_Device, ConsDepthTex[i].Get(), SRVDesc, ConsDepthSRV[i]);

            ConsDepthUAV[i] = _SRVHeap.Allocate(1);
            D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
            UAVDesc.Format        = DXGI_FORMAT_R16_FLOAT;
            UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            _SRVHeap.CreateUAV(_Device, ConsDepthTex[i].Get(), UAVDesc, ConsDepthUAV[i]);
        }
        BuildScatteringRootSignature(_Device);
        {
            auto CSO = LoadShaderBytecode("VolumetricFogScattering.cs_6_0.cso");
            D3D12_COMPUTE_PIPELINE_STATE_DESC Desc{};
            Desc.pRootSignature = ScatterRootSig.Get();
            Desc.CS             = { CSO.data(), CSO.size() };
            SMILE_HR(_Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&ScatterPSO)));
        }
        CreateConstantBuffer(_Device);

        Initialized = true;
        LogInfo("Volumetric fog (froxel 160x90x64) inicializado");
    }

    void FVolumetricFogPass::BuildScatteringRootSignature(ID3D12Device* _Device) {
        // Registers casam com VolumetricFogScattering: b0 = VolFogCB, b3/t11/s2 = CSM
        // (CSMCommon.hlsli), t0 = VBufferA, t1 = atlas de irradiancia do DDGI,
        // t2 = historia do scattering (ping-pong), t3 = lista FGPULight (root SRV),
        // t18+t19 = atlas spot + cube array (slots contiguos, uma tabela), u0 = saida.
        D3D12_DESCRIPTOR_RANGE VBufferRange{};
        VBufferRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        VBufferRange.NumDescriptors                    = 1;
        VBufferRange.BaseShaderRegister                = 0;
        VBufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE DDGIRange = VBufferRange;
        DDGIRange.BaseShaderRegister = 1;

        D3D12_DESCRIPTOR_RANGE HistoryRange = VBufferRange;
        HistoryRange.BaseShaderRegister = 2;

        D3D12_DESCRIPTOR_RANGE ShadowRange = VBufferRange;
        ShadowRange.BaseShaderRegister = 11;

        D3D12_DESCRIPTOR_RANGE LocalShadowRange = VBufferRange;
        LocalShadowRange.BaseShaderRegister = 18;
        LocalShadowRange.NumDescriptors     = 2; // t18 atlas spot + t19 cube array

        D3D12_DESCRIPTOR_RANGE CloudRange = VBufferRange;
        CloudRange.BaseShaderRegister = 4; // t4 shadow map das nuvens

        D3D12_DESCRIPTOR_RANGE ConsRange = VBufferRange;
        ConsRange.BaseShaderRegister = 5; // t5 cons depth atual

        D3D12_DESCRIPTOR_RANGE PrevConsRange = VBufferRange;
        PrevConsRange.BaseShaderRegister = 6; // t6 cons depth anterior

        D3D12_DESCRIPTOR_RANGE UAVRange{};
        UAVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        UAVRange.NumDescriptors                    = 1;
        UAVRange.BaseShaderRegister                = 0;
        UAVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER RootParams[12]{};
        RootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[0].Descriptor.ShaderRegister = 0;
        RootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        RootParams[1].Descriptor.ShaderRegister = 3;
        RootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[2].DescriptorTable.pDescriptorRanges   = &VBufferRange;
        RootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[3].DescriptorTable.pDescriptorRanges   = &DDGIRange;
        RootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[4].DescriptorTable.pDescriptorRanges   = &ShadowRange;
        RootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[5].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[5].DescriptorTable.pDescriptorRanges   = &HistoryRange;
        RootParams[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV; // t3 lista de luzes
        RootParams[6].Descriptor.ShaderRegister = 3;
        RootParams[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[7].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[7].DescriptorTable.pDescriptorRanges   = &LocalShadowRange;
        RootParams[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[8].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[8].DescriptorTable.pDescriptorRanges   = &UAVRange;
        RootParams[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[9].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[9].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[9].DescriptorTable.pDescriptorRanges   = &CloudRange;
        RootParams[9].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[10].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[10].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[10].DescriptorTable.pDescriptorRanges   = &ConsRange;
        RootParams[10].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        RootParams[11].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootParams[11].DescriptorTable.NumDescriptorRanges = 1;
        RootParams[11].DescriptorTable.pDescriptorRanges   = &PrevConsRange;
        RootParams[11].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC Samplers[2]{};
        Samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Samplers[0].MaxAnisotropy    = 1;
        Samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        Samplers[0].MinLOD           = 0.0f;
        Samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        Samplers[0].ShaderRegister   = 0;
        Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // identico ao s2 do deferred lighting/sun shafts: LESS_EQUAL + borda branca
        Samplers[1] = Samplers[0];
        Samplers[1].Filter         = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        Samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        Samplers[1].BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        Samplers[1].AddressU       = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        Samplers[1].AddressV       = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        Samplers[1].AddressW       = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        Samplers[1].ShaderRegister = 2;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(RootParams);
        Desc.pParameters       = RootParams;
        Desc.NumStaticSamplers = _countof(Samplers);
        Desc.pStaticSamplers   = Samplers;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, ErrorBlob;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &ErrorBlob);
        if (FAILED(Hr)) {
            if (ErrorBlob)
                LogError(std::string("VolFog scatter root sig error: ") +
                         static_cast<const char*>(ErrorBlob->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(_Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                              IID_PPV_ARGS(&ScatterRootSig)));
    }

    void FVolumetricFogPass::CreateConstantBuffer(ID3D12Device* _Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(VolFogConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
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

    // Halton radical-inverse (mesma base do VolumetricFogTemporalRandom da UE).
    static f32 Halton(u32 _Index, u32 _Base) {
        f32 R = 0.0f, F = 1.0f;
        while (_Index > 0) {
            F /= static_cast<f32>(_Base);
            R += F * static_cast<f32>(_Index % _Base);
            _Index /= _Base;
        }
        return R;
    }

    void FVolumetricFogPass::UpdatePerFrame(u32 _FrameSlot, const FFrameParams& _P) {
        FrameSlot = _FrameSlot;
        if (!MappedBase) return;

        // CalculateGridZParams da UE (RenderUtils.h): slice = log2(z*B+O)*S.
        // NearOffset 9.5cm empurra o primeiro slice pra fora do near plane.
        const f64 S = 32.0; // DepthDistributionScale (default UE)
        const f64 N = static_cast<f64>(_P.NearZ) + 0.095;
        const f64 F = static_cast<f64>(MaxDistance);
        const f64 O = (F - N * std::exp2(static_cast<f64>(kGridZ) / S)) / (F - N);
        const f64 B = (1.0 - O) / N;
        GridZ = { static_cast<f32>(B), static_cast<f32>(O), static_cast<f32>(S), (f32)kGridZ };

        VolFogConstants c{};
        c.GridZParams    = GridZ;
        c.GridSize       = { (f32)kGridW, (f32)kGridH, (f32)kGridZ, 0.0f };
        c.InvViewProj    = _P.InvViewProjUnjit;
        c.CameraWorldPos = { _P.CameraPos.X, _P.CameraPos.Y, _P.CameraPos.Z, ExtinctionScale };
        c.CamForward     = { _P.CameraForward.X, _P.CameraForward.Y, _P.CameraForward.Z, MaxDistance };
        const Vec3 SunN  = _P.DirToSun.NormalizedSafe(Vec3{ 0.3f, 0.6f, 0.5f }.Normalized());
        c.SunDirPhase    = { SunN.X, SunN.Y, SunN.Z, PhaseG };
        c.SunColorInt    = { _P.SunColorTimesIntensity.X, _P.SunColorTimesIntensity.Y,
                             _P.SunColorTimesIntensity.Z, 0.0f };
        c.FogDensityP    = _P.CollapsedFog;
        c.AlbedoAmb      = { Albedo.X, Albedo.Y, Albedo.Z, AmbientIntensity };
        c.DDGIGridMin    = _P.DDGIGridMin;
        c.DDGIGridCount  = _P.DDGIGridCount;
        c.DDGIParams     = _P.DDGIParams;
        c.AmbientFallback= { _P.SkyAmbient.X, _P.SkyAmbient.Y, _P.SkyAmbient.Z, 0.0f };

        // Temporal: historia valida so com PrevVP guardado E frame anterior ativo.
        const bool UseHistory = Temporal && HistoryValid && HasPrevFrame;
        c.PrevViewProj   = HasPrevFrame ? StoredPrevVP : _P.ViewProjUnjit;
        c.PrevCamPos     = { PrevCamPosV.X, PrevCamPosV.Y, PrevCamPosV.Z, 0.0f };
        c.PrevCamForward = { PrevCamFwdV.X, PrevCamFwdV.Y, PrevCamFwdV.Z, 0.0f };
        c.TemporalParams = { UseHistory ? kHistoryWeight : 0.0f,
                             Temporal ? (f32)kMissSupersamples : 1.0f, 0.0f, 0.0f };
        for (u32 i = 0; i < 8; ++i) {
            if (Temporal) {
                const u32 Fi = (_P.FrameIndex >= i ? _P.FrameIndex - i : 0u) & 1023u;
                c.FrameJitterOffsets[i] = { Halton(Fi, 2), Halton(Fi, 3), Halton(Fi, 5), 0.0f };
            } else {
                c.FrameJitterOffsets[i] = { 0.5f, 0.5f, 0.5f, 0.0f };
            }
        }

        // Luzes e nuvens: zerados aqui — os Patch* (pos-culling/pos-update das nuvens,
        // que rodam depois no frame) preenchem.
        c.LightParamsVF  = { 0.0f, 0.0f, 0.0f, LightsIntensity };
        c.LightParamsVF2 = { 0.05f, 0.0f, 0.0f, 0.0f };
        c.CloudShadowParams  = { 0.0f, 0.0f, 0.0f, 0.0f };
        c.CloudShadowParams2 = { 0.0f, 0.0f, 0.0f, 0.0f };

        c.CurViewProj     = _P.ViewProjUnjit;
        c.ConsDepthParams = { ConservativeDepth ? 1.0f : 0.0f,
                              static_cast<f32>(_P.RenderW), static_cast<f32>(_P.RenderH),
                              0.0f };

        // Guarda o frame atual como "anterior" do proximo.
        StoredPrevVP = _P.ViewProjUnjit;
        PrevCamPosV  = _P.CameraPos;
        PrevCamFwdV  = _P.CameraForward;
        HasPrevFrame = true;

        *Mapped() = c;
    }

    void FVolumetricFogPass::PatchLights(u32 _NumLights, f32 _InvSpotRes, f32 _DepthBias,
                                         f32 _PointNear) {
        if (!MappedBase) return;
        Mapped()->LightParamsVF  = { static_cast<f32>(_NumLights), _InvSpotRes, _DepthBias,
                                     LightsIntensity };
        Mapped()->LightParamsVF2 = { _PointNear, 0.0f, 0.0f, 0.0f };
    }

    void FVolumetricFogPass::PatchCloudShadow(const Vec4& _CloudShadowParams,
                                              const Vec4& _CloudShadowParams2) {
        if (!MappedBase) return;
        Mapped()->CloudShadowParams  = _CloudShadowParams;
        Mapped()->CloudShadowParams2 = _CloudShadowParams2;
    }

    void FVolumetricFogPass::Execute(ID3D12GraphicsCommandList* _CommandList,
                                     FTextureSRVHeap& _SRVHeap,
                                     D3D12_GPU_VIRTUAL_ADDRESS _CSMConstantsAddr,
                                     u32 _CSMShadowSRVSlot, u32 _DDGIIrradianceSRVSlot,
                                     D3D12_GPU_VIRTUAL_ADDRESS _LightsVA,
                                     u32 _LocalShadowSRVSlot, u32 _CloudShadowSRVSlot,
                                     u32 _DepthSRVSlot) {
        if (!Initialized) return;

        constexpr u32 GX = (kGridW + 3) / 4, GY = (kGridH + 3) / 4, GZ = (kGridZ + 3) / 4;

        // Barreira local dos ping-pong 2D do conservative depth.
        auto ConsTransition = [&](u32 Idx, D3D12_RESOURCE_STATES After) {
            if (ConsDepthState[Idx] == After) return;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = ConsDepthTex[Idx].Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = ConsDepthState[Idx];
            B.Transition.StateAfter  = After;
            _CommandList->ResourceBarrier(1, &B);
            ConsDepthState[Idx] = After;
        };

        // 0) Conservative depth: min-Z do tile (usa o ping-pong com o CurrentScatter —
        // cur escrito agora, prev vem do frame anterior pro fixup da historia).
        const u32 ConsCur = CurrentScatter, ConsPrev = 1u - CurrentScatter;
        if (ConservativeDepth) {
            ConsTransition(ConsCur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ConsDepthPSO.Bind(_CommandList);
            _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
            _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(_DepthSRVSlot));
            _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ConsDepthUAV[ConsCur]));
            _CommandList->Dispatch((kGridW + 7) / 8, (kGridH + 7) / 8, 1);
        }
        ConsTransition(ConsCur,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ConsTransition(ConsPrev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // 1) Densidade -> VBufferA
        VBufferA.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SetupPSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(Integrated.SRVSlot())); // t0 unused
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(VBufferA.UAVSlot(0)));
        _CommandList->Dispatch(GX, GY, GZ);

        // 2) Light scattering (sol CSM + ambiente DDGI + blend temporal) -> alvo do
        // ping-pong; a historia (frame anterior) entra em t2 como SRV.
        FVolumeTexture& Target  = LightScatteringPP[CurrentScatter];
        FVolumeTexture& History = LightScatteringPP[1u - CurrentScatter];
        VBufferA.Transition(_CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Target.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        History.Transition(_CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _CommandList->SetComputeRootSignature(ScatterRootSig.Get());
        _CommandList->SetPipelineState(ScatterPSO.Get());
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootConstantBufferView(1, _CSMConstantsAddr);
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(VBufferA.SRVSlot()));
        _CommandList->SetComputeRootDescriptorTable(3, _SRVHeap.GpuHandle(_DDGIIrradianceSRVSlot));
        _CommandList->SetComputeRootDescriptorTable(4, _SRVHeap.GpuHandle(_CSMShadowSRVSlot));
        _CommandList->SetComputeRootDescriptorTable(5, _SRVHeap.GpuHandle(History.SRVSlot()));
        _CommandList->SetComputeRootShaderResourceView(6, _LightsVA);
        _CommandList->SetComputeRootDescriptorTable(7, _SRVHeap.GpuHandle(_LocalShadowSRVSlot));
        _CommandList->SetComputeRootDescriptorTable(8, _SRVHeap.GpuHandle(Target.UAVSlot(0)));
        _CommandList->SetComputeRootDescriptorTable(9, _SRVHeap.GpuHandle(_CloudShadowSRVSlot));
        _CommandList->SetComputeRootDescriptorTable(10, _SRVHeap.GpuHandle(ConsDepthSRV[ConsCur]));
        _CommandList->SetComputeRootDescriptorTable(11, _SRVHeap.GpuHandle(ConsDepthSRV[ConsPrev]));
        _CommandList->Dispatch(GX, GY, GZ);

        // 3) Integracao acumulada -> Integrated (o fog fullscreen le em PIXEL)
        Target.Transition(_CommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Integrated.Transition(_CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        IntegratePSO.Bind(_CommandList);
        _CommandList->SetComputeRootConstantBufferView(0, CBAddr());
        _CommandList->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(Target.SRVSlot()));
        _CommandList->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(Integrated.UAVSlot(0)));
        _CommandList->Dispatch((kGridW + 7) / 8, (kGridH + 7) / 8, 1);

        Integrated.Transition(_CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // O alvo deste frame vira a historia do proximo.
        CurrentScatter = 1u - CurrentScatter;
        HistoryValid   = true;
    }
}
