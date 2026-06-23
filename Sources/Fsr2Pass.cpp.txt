#include "Smile/Graphics/Fsr2Pass.h"
#include "Smile/Core/Logger.h"

#if SMILE_FSR2_ENABLED
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include "ffx_fsr2.h"
#include "dx12/ffx_fsr2_dx12.h"
#endif

namespace Smile {

#if SMILE_FSR2_ENABLED

    namespace {
        constexpr u32         kInvalidSlot = 0xFFFFFFFFu;
        constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // mesmo do post chain

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

    struct FFsr2Pass::Impl {
        FfxFsr2Context    Context{};
        bool              Created = false;
        std::vector<char> Scratch;             // deve sobreviver enquanto o contexto existir
        u32 RW = 0, RH = 0, SW = 0, SH = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        u32  OutputSRV   = kInvalidSlot;        // alocado uma vez, reusado entre resizes
        bool FirstDispatch = true;

        void DestroyContext() {
            if (Created) { ffxFsr2ContextDestroy(&Context); Created = false; }
            Scratch.clear();
            Output.Reset();
        }
    };

    FFsr2Pass::FFsr2Pass() : P(std::make_unique<Impl>()) {}
    FFsr2Pass::~FFsr2Pass() { Shutdown(); }

    bool FFsr2Pass::Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               u32 RenderW, u32 RenderH, u32 DisplayW, u32 DisplayH) {
        if (!Device || RenderW == 0 || RenderH == 0 || DisplayW == 0 || DisplayH == 0) return false;
        P->DestroyContext();

        // --- Contexto FSR2 ---
        const size_t ScratchSize = ffxFsr2GetScratchMemorySizeDX12();
        P->Scratch.resize(ScratchSize);

        FfxFsr2Interface Iface{};
        FfxErrorCode Err = ffxFsr2GetInterfaceDX12(&Iface, Device, P->Scratch.data(), ScratchSize);
        if (Err != FFX_OK) {
            LogError("FSR2: ffxFsr2GetInterfaceDX12 falhou (codigo " + std::to_string(Err) + ")");
            P->Scratch.clear(); return false;
        }

        FfxFsr2ContextDescription Desc{};
        // HDR: HDRColorBuffer e R16F linear pre-tonemap. DEPTH_INVERTED: a engine usa Reverse-Z.
        // AUTO_EXPOSURE: por enquanto sem textura de exposicao (FSR2 calcula a propria).
        Desc.flags         = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE
                           | FFX_FSR2_ENABLE_DEPTH_INVERTED
                           | FFX_FSR2_ENABLE_AUTO_EXPOSURE;
        Desc.maxRenderSize = { RenderW, RenderH };
        Desc.displaySize   = { DisplayW, DisplayH };
        Desc.device        = ffxGetDeviceDX12(Device);
        Desc.callbacks     = Iface;
        Desc.fpMessage     = nullptr;

        Err = ffxFsr2ContextCreate(&P->Context, &Desc);
        if (Err != FFX_OK) {
            LogError("FSR2: ffxFsr2ContextCreate falhou (codigo " + std::to_string(Err) + ")");
            P->Scratch.clear(); return false;
        }

        // --- Textura de output (display-res, UAV p/ o FSR2 escrever + SRV p/ o post chain ler) ---
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
            LogError("FSR2: CreateCommittedResource (output) falhou");
            P->DestroyContext(); return false;
        }

        if (P->OutputSRV == kInvalidSlot) P->OutputSRV = SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
        SRVDesc.Format                  = kOutputFormat;
        SRVDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRVDesc.Texture2D.MipLevels     = 1;
        SRVHeap.CreateSRV(Device, P->Output.Get(), SRVDesc, P->OutputSRV);

        P->Created = true;
        P->FirstDispatch = true;
        P->RW = RenderW; P->RH = RenderH; P->SW = DisplayW; P->SH = DisplayH;
        LogInfo("FSR2 inicializado (render " + std::to_string(RenderW) + "x" + std::to_string(RenderH) +
                " -> display " + std::to_string(DisplayW) + "x" + std::to_string(DisplayH) + ")");
        return true;
    }

    void FFsr2Pass::Shutdown() { if (P) P->DestroyContext(); }
    bool FFsr2Pass::IsInitialized() const { return P && P->Created; }

    void FFsr2Pass::GetJitter(u32 FrameIndex, f32& OutX, f32& OutY) const {
        OutX = OutY = 0.0f;
        if (!P || !P->Created) return;
        const int32_t Phase = ffxFsr2GetJitterPhaseCount(static_cast<int32_t>(P->RW),
                                                         static_cast<int32_t>(P->SW));
        if (Phase <= 0) return;
        ffxFsr2GetJitterOffset(&OutX, &OutY,
                               static_cast<int32_t>(FrameIndex % static_cast<u32>(Phase)), Phase);
    }

    void FFsr2Pass::Dispatch(ID3D12GraphicsCommandList* Cmd,
                             ID3D12Resource* Color, ID3D12Resource* Depth, ID3D12Resource* Velocity,
                             f32 JitterX, f32 JitterY,
                             f32 NearZ, f32 FarZ, f32 FovYRadians,
                             f32 DeltaTimeSec, bool Reset) {
        if (!P || !P->Created || !Cmd || !Color || !Depth || !Velocity) return;

        // Output: PIXEL_SHADER_RESOURCE -> UNORDERED_ACCESS para o FSR2 escrever.
        Transition(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        P->OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        // Inputs: o chamador ja os colocou em NON_PIXEL_SHADER_RESOURCE (= COMPUTE_READ).
        FfxFsr2DispatchDescription D{};
        D.commandList   = ffxGetCommandListDX12(Cmd);
        D.color         = ffxGetResourceDX12(&P->Context, Color,    L"FSR2_Color",    FFX_RESOURCE_STATE_COMPUTE_READ);
        D.depth         = ffxGetResourceDX12(&P->Context, Depth,    L"FSR2_Depth",    FFX_RESOURCE_STATE_COMPUTE_READ);
        D.motionVectors = ffxGetResourceDX12(&P->Context, Velocity, L"FSR2_Velocity", FFX_RESOURCE_STATE_COMPUTE_READ);
        D.exposure                   = FfxResource{};   // AUTO_EXPOSURE -> nao fornecida
        D.reactive                   = FfxResource{};   // Fase 4
        D.transparencyAndComposition = FfxResource{};   // Fase 4
        D.output        = ffxGetResourceDX12(&P->Context, P->Output.Get(), L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        D.jitterOffset      = { JitterX, JitterY };           // o MESMO jitter aplicado a projecao
        // velocity da engine = curUV - prevUV (UV, y-down). FSR2 reprojeta esperando o vetor
        // apontando para o frame ANTERIOR em pixels -> negar + UV->pixels. GOTCHA #1: se o smear
        // sair na direcao oposta ao movimento, inverter o sinal aqui (ou no GBuffer.ps).
        D.motionVectorScale = { -static_cast<float>(P->RW), -static_cast<float>(P->RH) };
        D.renderSize        = { P->RW, P->RH };
        D.enableSharpening  = false;                          // RCAS fica p/ Fase 4
        D.sharpness         = 0.0f;
        D.frameTimeDelta    = (DeltaTimeSec > 0.0f ? DeltaTimeSec : 1.0f / 60.0f) * 1000.0f; // ms
        D.preExposure       = 1.0f;                           // deve ser > 0
        D.reset             = Reset || P->FirstDispatch;
        D.cameraNear        = NearZ;
        D.cameraFar         = FarZ;
        D.cameraFovAngleVertical = FovYRadians;

        FfxErrorCode Err = ffxFsr2ContextDispatch(&P->Context, &D);
        if (Err != FFX_OK)
            LogError("FSR2: ffxFsr2ContextDispatch falhou (codigo " + std::to_string(Err) + ")");

        // Output: UNORDERED_ACCESS -> PIXEL_SHADER_RESOURCE para o post chain ler.
        Transition(Cmd, P->Output.Get(), P->OutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        P->OutputState   = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        P->FirstDispatch = false;
    }

    ID3D12Resource* FFsr2Pass::OutputResource() const { return P ? P->Output.Get() : nullptr; }
    u32             FFsr2Pass::OutputSRVSlot() const { return P ? P->OutputSRV : kInvalidSlot; }
    u32 FFsr2Pass::RenderW()  const { return P ? P->RW : 0; }
    u32 FFsr2Pass::RenderH()  const { return P ? P->RH : 0; }
    u32 FFsr2Pass::DisplayW() const { return P ? P->SW : 0; }
    u32 FFsr2Pass::DisplayH() const { return P ? P->SH : 0; }

#else  // ============================ stub (Debug ou SDK ausente) ================================

    struct FFsr2Pass::Impl {};
    FFsr2Pass::FFsr2Pass()  = default;
    FFsr2Pass::~FFsr2Pass() = default;
    bool FFsr2Pass::Initialize(ID3D12Device*, FTextureSRVHeap&, u32, u32, u32, u32) { return false; }
    void FFsr2Pass::Shutdown() {}
    bool FFsr2Pass::IsInitialized() const { return false; }
    void FFsr2Pass::GetJitter(u32, f32& OutX, f32& OutY) const { OutX = OutY = 0.0f; }
    void FFsr2Pass::Dispatch(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*,
                             ID3D12Resource*, f32, f32, f32, f32, f32, f32, bool) {}
    ID3D12Resource* FFsr2Pass::OutputResource() const { return nullptr; }
    u32             FFsr2Pass::OutputSRVSlot() const { return 0xFFFFFFFFu; }
    u32 FFsr2Pass::RenderW()  const { return 0; }
    u32 FFsr2Pass::RenderH()  const { return 0; }
    u32 FFsr2Pass::DisplayW() const { return 0; }
    u32 FFsr2Pass::DisplayH() const { return 0; }

#endif
}
