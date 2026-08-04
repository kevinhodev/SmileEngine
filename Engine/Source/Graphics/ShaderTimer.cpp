#include "Smile/Graphics/ShaderTimer.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"

#if SMILE_NVAPI_ENABLED
// Ordem OBRIGATORIA: o nvapi.h so expoe a parte de D3D12 se o d3d12.h ja tiver sido incluido (ele
// testa __d3d12_h__ — vem do ShaderTimer.h), e as assinaturas dessa secao mencionam IDXGISwapChain,
// que o d3d12.h NAO arrasta. Sem o dxgi aqui o header quebra em C2061 dentro do proprio nvapi.h.
#include <dxgi1_6.h>
#include <nvapi.h>
#endif

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kTimerFormat = DXGI_FORMAT_R32_FLOAT;

        // Estado da extensao: global porque o slot falso e uma propriedade do DEVICE, nao de um
        // passe. Uma instancia de FShaderTimer por passe, mas um unico slot p/ todos.
        bool                   ApiReady   = false;
        u32                    DummySlot  = FShaderTimer::kInvalidSlot;
        ComPtr<ID3D12Resource> DummyBuffer;

        // O UAV falso e declarado no HLSL como RWStructuredBuffer<NvShaderExtnStruct> (256 B por
        // elemento). SEM contador, apesar do __NvGetSpecial escrever IncrementCounter(): o driver
        // remove o acesso inteiro ao reconhecer o padrao, entao contador nenhum e usado de fato —
        // e o CreateUAV da engine passa pCounterResource = nullptr, caso em que o D3D12 EXIGE
        // CounterOffsetInBytes = 0. Pedir offset com counter nulo e chamada invalida: o device
        // some ali mesmo e o erro so aparece no proximo CreateRootSignature, como DEVICE_REMOVED.
        constexpr u32 kExtnStructStride = 256;
    }

    bool FShaderTimer::IsAvailable() { return ApiReady; }

    bool FShaderTimer::InitializeApi(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
#if SMILE_NVAPI_ENABLED
        if (ApiReady) return true;
        if (!_Device) return false;

        if (NvAPI_Initialize() != NVAPI_OK) {
            LogInfo("NVAPI indisponivel — instrumentacao de timer nos shaders de RT desligada.");
            return false;
        }
        // Reserva u999/space0 no device. Vale p/ toda PSO criada daqui em diante; quem nao
        // declara o slot no HLSL nao paga nada.
        if (NvAPI_D3D12_SetNvShaderExtnSlotSpace(_Device, kExtnSlot, kExtnSpace) != NVAPI_OK) {
            LogWarning("NvAPI_D3D12_SetNvShaderExtnSlotSpace falhou — timer de shader desligado.");
            NvAPI_Unload();
            return false;
        }

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = kExtnStructStride;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&DummyBuffer)))) {
            LogWarning("Buffer do slot da NVAPI nao pode ser criado — timer de shader desligado.");
            NvAPI_Unload();
            return false;
        }
        VramTracker::Register(DummyBuffer.Get(), EVramCategory::Misc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension                = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Format                       = DXGI_FORMAT_UNKNOWN;
        Uav.Buffer.NumElements           = 1;
        Uav.Buffer.StructureByteStride   = kExtnStructStride;
        Uav.Buffer.CounterOffsetInBytes  = 0;
        DummySlot = _SRVHeap.Allocate(1);
        _SRVHeap.CreateUAV(_Device, DummyBuffer.Get(), Uav, DummySlot);

        ApiReady = true;
        LogInfo("NVAPI ligada: instrumentacao de timer nos shaders de RT disponivel.");
        return true;
#else
        (void)_Device; (void)_SRVHeap;
        return false;
#endif
    }

    void FShaderTimer::ShutdownApi() {
#if SMILE_NVAPI_ENABLED
        if (!ApiReady) return;
        // ~0u desliga o slot falso — o inverso do SetNvShaderExtnSlotSpace, como no artigo.
        NvAPI_D3D12_SetNvShaderExtnSlotSpace(nullptr, ~0u, ~0u);
        NvAPI_Unload();
#endif
        DummyBuffer.Reset();
        DummySlot = kInvalidSlot;
        ApiReady  = false;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE FShaderTimer::ExtnTable(const FTextureSRVHeap& _SRVHeap) {
        return _SRVHeap.GpuHandle(DummySlot);
    }

    void FShaderTimer::Initialize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                  u32 _Width, u32 _Height) {
        Release(_SRVHeap);
        if (!ApiReady || _Width == 0 || _Height == 0) return;

        // R32_FLOAT e nao R32_UINT: o visualizador le todo alvo como Texture2D<float4>, e o
        // heatmap so precisa de ordem de grandeza — ciclos cabem folgadamente no mantissa.
        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = _Width;
        Desc.Height           = _Height;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = kTimerFormat;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Target)));
        VramTracker::Register(Target.Get(), EVramCategory::Misc);
        State = D3D12_RESOURCE_STATE_COMMON;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                  = kTimerFormat;
        Srv.Texture2D.MipLevels     = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kTimerFormat;

        SRVSlot = _SRVHeap.Allocate(1);
        UAVSlot = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, Target.Get(), Srv, SRVSlot);
        _SRVHeap.CreateUAV(_Device, Target.Get(), Uav, UAVSlot);
    }

    void FShaderTimer::Release(FTextureSRVHeap& _SRVHeap) {
        if (SRVSlot != kInvalidSlot) { _SRVHeap.Free(SRVSlot); SRVSlot = kInvalidSlot; }
        if (UAVSlot != kInvalidSlot) { _SRVHeap.Free(UAVSlot); UAVSlot = kInvalidSlot; }
        Target.Reset();
        State = D3D12_RESOURCE_STATE_COMMON;
    }

    void FShaderTimer::Clear(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Target) return;
        if (State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = Target.Get();
            B.Transition.StateBefore = State;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            _CL->ResourceBarrier(1, &B);
            State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        const float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        _CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(UAVSlot),
                                           _SRVHeap.CpuHandleStaging(UAVSlot),
                                           Target.Get(), Zero, 0, nullptr);
    }

    void FShaderTimer::ToRead(ID3D12GraphicsCommandList* _CL) {
        if (!Target) return;
        // O visualizador e um passe grafico e o modo "grade" pode ler junto com outros alvos:
        // termina no estado combinado, como o resto dos alvos publicados no DebugTargets.
        const D3D12_RESOURCE_STATES After = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (State == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Target.Get();
        B.Transition.StateBefore = State;
        B.Transition.StateAfter  = After;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _CL->ResourceBarrier(1, &B);
        State = After;
    }
}
