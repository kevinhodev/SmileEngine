#include "Smile/Graphics/TemporalAA.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Graphics/CommandQueue.h"
#include <cstring>

namespace Smile {
    namespace {
        void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            if (before == after) return;
            D3D12_RESOURCE_BARRIER b{};
            b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter  = after;
            cl->ResourceBarrier(1, &b);
        }
        constexpr DXGI_FORMAT kHDRFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    void FTemporalAA::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H) {
        if (Initialized) return;
        BuildRootSignature(Device);
        BuildPSO(Device);
        CreateConstantBuffer(Device);
        CreateTextures(Device, SRVHeap, W, H);
        Initialized = true;
        LogDebug("TemporalAA (reprojecao so-camera + Karis tonemap-weighted) inicializado");
    }

    void FTemporalAA::Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H) {
        if (!Initialized) return;
        CreateTextures(Device, SRVHeap, W, H);
    }

    void FTemporalAA::CreateTextures(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 W, u32 H) {
        Width = W; Height = H;
        if (W == 0 || H == 0) return;

        History[0].Reset();
        History[1].Reset();
        DisplayTex.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{}; HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = W;
        Desc.Height           = H;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = kHDRFormat;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        D3D12_CLEAR_VALUE ClearValue{};
        ClearValue.Format = kHDRFormat;
        std::memcpy(ClearValue.Color, ClearColor, sizeof(ClearColor));

        for (int i = 0; i < 2; ++i) {
            SMILE_HR(Device->CreateCommittedResource(
                &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
                IID_PPV_ARGS(&History[i])));
            VramTracker::Register(History[i].Get(), EVramCategory::RenderTargets);
            HistoryState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        SMILE_HR(Device->CreateCommittedResource(
            &HeapProps, D3D12_HEAP_FLAG_NONE, &Desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
            IID_PPV_ARGS(&DisplayTex)));
        VramTracker::Register(DisplayTex.Get(), EVramCategory::RenderTargets);
        DisplayState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!HistoryRTVHeap.Native())
            HistoryRTVHeap.Initialize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, false);

        D3D12_RENDER_TARGET_VIEW_DESC RTVDesc{};
        RTVDesc.Format        = kHDRFormat;
        RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (int i = 0; i < 2; ++i)
            Device->CreateRenderTargetView(History[i].Get(), &RTVDesc, HistoryRTVHeap.CpuHandle(i));
        Device->CreateRenderTargetView(DisplayTex.Get(), &RTVDesc, HistoryRTVHeap.CpuHandle(2));

        if (HistorySRVBase == kInvalidSlot)
            HistorySRVBase = SRVHeap.Allocate(2);
        if (DisplaySRVSlot == kInvalidSlot)
            DisplaySRVSlot = SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = kHDRFormat;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        for (int i = 0; i < 2; ++i)
            SRVHeap.CreateSRV(Device, History[i].Get(), SRVDesc, HistorySRVBase + i);
        SRVHeap.CreateSRV(Device, DisplayTex.Get(), SRVDesc, DisplaySRVSlot);

        HistoryIndex    = 0;
        LastOutputIndex = 0;
    }

    void FTemporalAA::SetupInputs(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  ID3D12Resource* HDRInput, ID3D12Resource* Depth,
                                  ID3D12Resource* VelocityTex) {
        if (!Initialized) return;
        if (ResolveTableBase == kInvalidSlot)
            ResolveTableBase = SRVHeap.Allocate(8); // 2 paridades x 4 slots [HDR, history, depth, velocity]

        D3D12_SHADER_RESOURCE_VIEW_DESC HDRSrv{};
        HDRSrv.Format                  = kHDRFormat;
        HDRSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        HDRSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        HDRSrv.Texture2D.MipLevels     = 1;

        D3D12_SHADER_RESOURCE_VIEW_DESC DepthSrv = HDRSrv;
        DepthSrv.Format = DXGI_FORMAT_R32_FLOAT;

        D3D12_SHADER_RESOURCE_VIEW_DESC VelSrv = HDRSrv;
        VelSrv.Format = DXGI_FORMAT_R16G16_FLOAT;

        for (u32 i = 0; i < 2; ++i) {
            const u32 base = ResolveTableBase + i * 4;
            SRVHeap.CreateSRV(Device, HDRInput,            HDRSrv,   base + 0);
            SRVHeap.CreateSRV(Device, History[1 - i].Get(), HDRSrv,  base + 1);
            SRVHeap.CreateSRV(Device, Depth,               DepthSrv, base + 2);
            SRVHeap.CreateSRV(Device, VelocityTex,         VelSrv,   base + 3);
        }
    }

    void FTemporalAA::BuildRootSignature(ID3D12Device* Device) {
        D3D12_DESCRIPTOR_RANGE SRVRange{};
        SRVRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        SRVRange.NumDescriptors                    = 4; // t0 HDR, t1 history, t2 depth, t3 velocity
        SRVRange.BaseShaderRegister                = 0;
        SRVRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER Params[2]{};
        Params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        Params[0].Descriptor.ShaderRegister = 0;
        Params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        Params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[1].DescriptorTable.NumDescriptorRanges = 1;
        Params[1].DescriptorTable.pDescriptorRanges   = &SRVRange;
        Params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC Sampler{};
        Sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        Sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        Sampler.ShaderRegister   = 0;
        Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC Desc{};
        Desc.NumParameters     = _countof(Params);
        Desc.pParameters       = Params;
        Desc.NumStaticSamplers = 1;
        Desc.pStaticSamplers   = &Sampler;
        Desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> Blob, Err;
        HRESULT Hr = D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Err);
        if (FAILED(Hr)) {
            if (Err) LogError(std::string("TAA Root Sig Error: ") +
                              static_cast<const char*>(Err->GetBufferPointer()));
            SMILE_HR(Hr);
        }
        SMILE_HR(Device->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(),
                                             IID_PPV_ARGS(&RootSig)));
    }

    void FTemporalAA::BuildPSO(ID3D12Device* Device) {
        auto VS = LoadShaderBytecode("PostProcess.vs_6_0.cso");
        auto PS = LoadShaderBytecode("TAAResolve.ps_6_0.cso");

        D3D12_RASTERIZER_DESC Raster{};
        Raster.FillMode        = D3D12_FILL_MODE_SOLID;
        Raster.CullMode        = D3D12_CULL_MODE_NONE;
        Raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC Blend{};
        Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        Blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; // SV_Target1 = debug

        D3D12_DEPTH_STENCIL_DESC Depth{};
        Depth.DepthEnable    = FALSE;
        Depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature        = RootSig.Get();
        PSODesc.VS                    = { VS.data(), VS.size() };
        PSODesc.PS                    = { PS.data(), PS.size() };
        PSODesc.BlendState            = Blend;
        PSODesc.SampleMask            = UINT_MAX;
        PSODesc.RasterizerState       = Raster;
        PSODesc.DepthStencilState     = Depth;
        PSODesc.InputLayout           = { nullptr, 0 };
        PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        PSODesc.NumRenderTargets      = 2;             // [0] history (feedback), [1] debug
        PSODesc.RTVFormats[0]         = kHDRFormat;
        PSODesc.RTVFormats[1]         = kHDRFormat;
        PSODesc.SampleDesc            = { 1, 0 };
        SMILE_HR(Device->CreateGraphicsPipelineState(&PSODesc, IID_PPV_ARGS(&ResolvePSO)));
    }

    void FTemporalAA::CreateConstantBuffer(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES UploadHeap{}; UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = sizeof(TAAConstants) * FCommandQueue::kFramesInFlight;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        SMILE_HR(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));
    }

    void FTemporalAA::Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                              u32 FrameSlot, const Mat44& InvViewProj, const Mat44& PrevViewProj,
                              f32 HistoryBlend, bool HistoryValid, f32 VarianceGamma, f32 Sharpness,
                              f32 MotionBlend, f32 AntiFlicker, f32 StationaryMargin,
                              const Vec3& CameraPosition, f32 NearZ, f32 FarZ, u32 DebugMode) {
        if (!Initialized || Width == 0 || Height == 0) return;

        const u32 curr = HistoryIndex;

        TAAConstants C{};
        C.InvViewProj  = InvViewProj;
        C.PrevViewProj = PrevViewProj;
        C.Params0      = { 1.0f / static_cast<f32>(Width), 1.0f / static_cast<f32>(Height),
                           HistoryBlend, HistoryValid ? 1.0f : 0.0f };
        C.Params1      = { VarianceGamma, Sharpness, static_cast<f32>(DebugMode), MotionBlend };
        C.Params2      = { AntiFlicker, StationaryMargin, NearZ, FarZ };
        C.CameraPos    = { CameraPosition.X, CameraPosition.Y, CameraPosition.Z, 1.0f };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(TAAConstants),
                    &C, sizeof(TAAConstants));

        Transition(CommandList, History[curr].Get(), HistoryState[curr], D3D12_RESOURCE_STATE_RENDER_TARGET);
        HistoryState[curr] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Transition(CommandList, DisplayTex.Get(), DisplayState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        DisplayState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        D3D12_VIEWPORT VP{}; VP.Width = static_cast<FLOAT>(Width); VP.Height = static_cast<FLOAT>(Height);
        VP.MinDepth = 0.0f; VP.MaxDepth = 1.0f;
        D3D12_RECT Sc{}; Sc.right = static_cast<LONG>(Width); Sc.bottom = static_cast<LONG>(Height);
        CommandList->RSSetViewports(1, &VP);
        CommandList->RSSetScissorRects(1, &Sc);

        // MRT: SV_Target0 -> history[curr] (feedback), SV_Target1 -> DisplayTex (so tela).
        D3D12_CPU_DESCRIPTOR_HANDLE RTVs[2] = { HistoryRTVHeap.CpuHandle(curr), HistoryRTVHeap.CpuHandle(2) };
        CommandList->OMSetRenderTargets(2, RTVs, FALSE, nullptr);

        CommandList->SetGraphicsRootSignature(RootSig.Get());
        CommandList->SetPipelineState(ResolvePSO.Get());
        CommandList->SetGraphicsRootConstantBufferView(
            0, CB->GetGPUVirtualAddress() + static_cast<u64>(FrameSlot) * sizeof(TAAConstants));
        CommandList->SetGraphicsRootDescriptorTable(1, SRVHeap.GpuHandle(ResolveTableBase + curr * 4));

        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 0, nullptr);
        CommandList->IASetIndexBuffer(nullptr);
        CommandList->DrawInstanced(3, 1, 0, 0);

        Transition(CommandList, History[curr].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        HistoryState[curr] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Transition(CommandList, DisplayTex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        DisplayState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        LastOutputIndex = curr;
        HistoryIndex    = 1 - curr;
    }
}
