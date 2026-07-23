#include "Smile/Graphics/FsrPass.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/Logger.h"

#if SMILE_FSR_ENABLED
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/dx12/ffx_api_dx12.h>
#endif

namespace Smile {

#if SMILE_FSR_ENABLED

    namespace {
        constexpr u32         kInvalidSlot = 0xFFFFFFFFu;
        constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        void Transition(ID3D12GraphicsCommandList* Cmd, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After) {
            if (Before == After) return;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = Res;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = Before;
            B.Transition.StateAfter  = After;
            Cmd->ResourceBarrier(1, &B);
        }
    }

    struct FFsrPass::Impl {
        ffxContext Context = nullptr;          // upscaler context do ffx-api (void*)
        bool       Created = false;
        u32 RW = 0, RH = 0, SW = 0, SH = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        u32  OutputSRV   = kInvalidSlot;
        bool FirstDispatch = true;

        void DestroyContext() {
            if (Created) { ffxDestroyContext(&Context, nullptr); Created = false; }
            Context = nullptr;
            Output.Reset();
        }
    };

    FFsrPass::FFsrPass() : P(std::make_unique<Impl>()) {}
    FFsrPass::~FFsrPass() { Shutdown(); }

    bool FFsrPass::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                              u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) {
        if (!Device || RenderW == 0 || RenderH == 0 || DisplayW == 0 || DisplayH == 0) return false;
        P->DestroyContext();

        // Backend DX12 encadeado como pNext do desc de criacao do upscaler.
        ffxCreateBackendDX12Desc BackendDesc{};
        BackendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        BackendDesc.device      = Device;

        ffxCreateContextDescUpscale CreateDesc{};
        CreateDesc.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        CreateDesc.header.pNext   = &BackendDesc.header;
        CreateDesc.flags          = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE
                                  | FFX_UPSCALE_ENABLE_DEPTH_INVERTED
                                  | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        CreateDesc.maxRenderSize  = { RenderW, RenderH };
        CreateDesc.maxUpscaleSize = { DisplayW, DisplayH };
        CreateDesc.fpMessage      = nullptr;

        ffxReturnCode_t Rc = ffxCreateContext(&P->Context, &CreateDesc.header, nullptr);
        if (Rc != FFX_API_RETURN_OK) {
            LogError("FSR: ffxCreateContext falhou (codigo " + std::to_string(Rc) + ")");
            P->Context = nullptr;
            return false;
        }
        P->Created = true;

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC TexDesc{};
        TexDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TexDesc.Width            = DisplayW;
        TexDesc.Height           = DisplayH;
        TexDesc.DepthOrArraySize = 1;
        TexDesc.MipLevels        = 1;
        TexDesc.Format           = kOutputFormat;
        TexDesc.SampleDesc       = { 1, 0 };
        TexDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        TexDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        P->OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        HRESULT Hr = Device->CreateCommittedResource(
            &Heap, D3D12_HEAP_FLAG_NONE, &TexDesc, P->OutputState, nullptr,
            IID_PPV_ARGS(&P->Output));
        if (FAILED(Hr)) {
            LogError("FSR: CreateCommittedResource (output) falhou");
            P->DestroyContext(); return false;
        }
        VramTracker::Register(P->Output.Get(), EVramCategory::RenderTargets);

        if (P->OutputSRV == kInvalidSlot) P->OutputSRV = SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = kOutputFormat;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device, P->Output.Get(), SRVDesc, P->OutputSRV);

        P->FirstDispatch = true;
        P->RW = RenderW; P->RH = RenderH; P->SW = DisplayW; P->SH = DisplayH;
        LogInfo("FSR (ffx-api / FSR 3.1) inicializado (render " + std::to_string(RenderW) + "x" +
                std::to_string(RenderH) + " -> display " + std::to_string(DisplayW) + "x" +
                std::to_string(DisplayH) + ")");
        return true;
    }

    void FFsrPass::Shutdown() { if (P) P->DestroyContext(); }
    bool FFsrPass::IsInitialized() const { return P && P->Created; }

    void FFsrPass::GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const {
        OutX = OutY = 0.0f;
        if (!P || !P->Created) return;

        int32_t Phase = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount PhaseDesc{};
        PhaseDesc.header.type    = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        PhaseDesc.renderWidth    = P->RW;
        PhaseDesc.displayWidth   = P->SW;
        PhaseDesc.pOutPhaseCount = &Phase;
        if (ffxQuery(&P->Context, &PhaseDesc.header) != FFX_API_RETURN_OK || Phase <= 0) return;

        ffxQueryDescUpscaleGetJitterOffset JitterDesc{};
        JitterDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        JitterDesc.index       = static_cast<int32_t>(FrameIndex % static_cast<u32>(Phase));
        JitterDesc.phaseCount  = Phase;
        JitterDesc.pOutX       = &OutX;
        JitterDesc.pOutY       = &OutY;
        ffxQuery(&P->Context, &JitterDesc.header);
    }

    void FFsrPass::Dispatch(ID3D12GraphicsCommandList* Cmd,
                            ID3D12Resource* Color, ID3D12Resource* Depth, ID3D12Resource* Velocity,
                            f32 JitterX, f32 JitterY,
                            f32 NearZ, f32 FarZ, f32 FovYRadians,
                            f32 DeltaTimeSec, bool Reset) {
        if (!P || !P->Created || !Cmd || !Color || !Depth || !Velocity) return;

        Transition(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        P->OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        // O estado passado em cada FfxApiResource deve refletir o estado ATUAL do recurso: o chamador
        // poe Color/Depth/Velocity em NON_PIXEL_SHADER_RESOURCE (= COMPUTE_READ) antes do Dispatch.
        ffxDispatchDescUpscale D{};
        D.header.type   = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        D.commandList   = Cmd;
        D.color         = ffxApiGetResourceDX12(Color,    FFX_API_RESOURCE_STATE_COMPUTE_READ);
        D.depth         = ffxApiGetResourceDX12(Depth,    FFX_API_RESOURCE_STATE_COMPUTE_READ);
        D.motionVectors = ffxApiGetResourceDX12(Velocity, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        D.output        = ffxApiGetResourceDX12(P->Output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        D.exposure                   = FfxApiResource{};   // auto-exposure ligado -> sem recurso externo
        D.reactive                   = FfxApiResource{};
        D.transparencyAndComposition = FfxApiResource{};

        D.jitterOffset            = { JitterX, JitterY };
        // Motion vectors da engine em UV*(-render); convencao preservada da integracao anterior.
        D.motionVectorScale       = { -static_cast<float>(P->RW), -static_cast<float>(P->RH) };
        D.renderSize              = { P->RW, P->RH };
        D.upscaleSize             = { P->SW, P->SH };
        D.enableSharpening        = false;                 // sharpen fica fora do FSR (feito no post chain)
        D.sharpness               = 0.0f;
        D.frameTimeDelta          = (DeltaTimeSec > 0.0f ? DeltaTimeSec : 1.0f / 60.0f) * 1000.0f; // ms
        D.preExposure             = 1.0f;
        D.reset                   = Reset || P->FirstDispatch;
        D.cameraNear              = NearZ;
        D.cameraFar               = FarZ;
        D.cameraFovAngleVertical  = FovYRadians;
        D.viewSpaceToMetersFactor = 0.0f;
        D.flags                   = 0;

        ffxReturnCode_t Rc = ffxDispatch(&P->Context, &D.header);
        if (Rc != FFX_API_RETURN_OK)
            LogError("FSR: ffxDispatch falhou (codigo " + std::to_string(Rc) + ")");

        Transition(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        P->OutputState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        P->FirstDispatch = false;
    }

    ID3D12Resource* FFsrPass::OutputResource() const { return P ? P->Output.Get() : nullptr; }
    u32             FFsrPass::OutputSRVSlot() const { return P ? P->OutputSRV : kInvalidSlot; }
    u32 FFsrPass::RenderW()  const { return P ? P->RW : 0; }
    u32 FFsrPass::RenderH()  const { return P ? P->RH : 0; }
    u32 FFsrPass::DisplayW() const { return P ? P->SW : 0; }
    u32 FFsrPass::DisplayH() const { return P ? P->SH : 0; }

#else
    struct FFsrPass::Impl {};
    FFsrPass::FFsrPass()  = default;
    FFsrPass::~FFsrPass() = default;
    bool FFsrPass::Initialize(ID3D12Device*, FTextureSRVHeap&, u32, u32, u32, u32) { return false; }
    void FFsrPass::Shutdown() {}
    bool FFsrPass::IsInitialized() const { return false; }
    void FFsrPass::GetJitter(u32, f32& OutX, f32& OutY) const { OutX = OutY = 0.0f; }
    void FFsrPass::Dispatch(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*,
                            ID3D12Resource*, f32, f32, f32, f32, f32, f32, bool) {}
    ID3D12Resource* FFsrPass::OutputResource() const { return nullptr; }
    u32             FFsrPass::OutputSRVSlot() const { return 0xFFFFFFFFu; }
    u32 FFsrPass::RenderW()  const { return 0; }
    u32 FFsrPass::RenderH()  const { return 0; }
    u32 FFsrPass::DisplayW() const { return 0; }
    u32 FFsrPass::DisplayH() const { return 0; }

#endif
}
