#include "Smile/Graphics/GIFallback.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {
    namespace {
        // Os MESMOS formatos dos recursos reais do DDGI. Nao e cosmetico: o SRV que a tabela do
        // consumidor recebe descreve um formato, e o shader declara Texture2D<float4> /
        // Texture2D<float2>. Um neutro em formato diferente daria descritor incompativel com a
        // declaracao — o tipo de erro que so aparece como leitura corrompida quando alguem
        // reabrir o gate.
        constexpr DXGI_FORMAT kIrradFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr DXGI_FORMAT kDistFormat  = DXGI_FORMAT_R16G16_FLOAT;
        constexpr DXGI_FORMAT kProbeFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

        // Igual ao kAtlasRead do FDDGI: o atlas e lido tanto pelo deferred (PIXEL) quanto pelos
        // traces (NON_PIXEL). Os neutros entram nesse estado e NUNCA saem — nada os escreve depois
        // do clear inicial, entao nao ha transicao por frame para esquecer.
        constexpr D3D12_RESOURCE_STATES kRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    void FGIFallbackResources::Initialize(ID3D12Device* _Device, FCommandQueue& _Queue,
                                          FTextureSRVHeap& _SRVHeap) {
        if (Ready || !_Device) return;
        Release(_SRVHeap);

        IrradTex = GpuResources::CreateTex2D(
            _Device, 1, 1, kIrradFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI, nullptr, 1, 1,
            "GI fallback · irradiancia neutra");
        DistTex = GpuResources::CreateTex2D(
            _Device, 1, 1, kDistFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI, nullptr, 1, 1,
            "GI fallback · distancia neutra");
        ProbeBuf = GpuResources::CreateBuffer(
            _Device, sizeof(f32) * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI, "GI fallback · ProbeData neutro");
        if (!IrradTex || !DistTex || !ProbeBuf) { Release(_SRVHeap); return; }

        IrradSRV     = _SRVHeap.Allocate(1);
        DistSRV      = _SRVHeap.Allocate(1);
        ProbeDataSRV = _SRVHeap.Allocate(1);
        // UAVs SO para o clear inicial. Devolvidos ao heap no fim desta funcao: manter tres slots
        // vivos por toda a sessao para escrever uma vez seria desperdicio de um recurso escasso.
        const u32 UavBase = _SRVHeap.Allocate(3);

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Texture2D.MipLevels     = 1;
        Srv.Format = kIrradFormat;
        _SRVHeap.CreateSRV(_Device, IrradTex.Get(), Srv, IrradSRV);
        Srv.Format = kDistFormat;
        _SRVHeap.CreateSRV(_Device, DistTex.Get(), Srv, DistSRV);

        Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Format                     = kProbeFormat;
        Srv.Buffer.FirstElement        = 0;
        Srv.Buffer.NumElements         = 1;
        Srv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeBuf.Get(), Srv, ProbeDataSRV);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kIrradFormat;
        _SRVHeap.CreateUAV(_Device, IrradTex.Get(), Uav, UavBase + 0);
        Uav.Format        = kDistFormat;
        _SRVHeap.CreateUAV(_Device, DistTex.Get(), Uav, UavBase + 1);
        Uav.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Format              = kProbeFormat;
        Uav.Buffer.FirstElement = 0;
        Uav.Buffer.NumElements  = 1;
        _SRVHeap.CreateUAV(_Device, ProbeBuf.Get(), Uav, UavBase + 2);

        _Queue.ResetForRecording();
        ID3D12GraphicsCommandList* CL = _Queue.List();
        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CL->SetDescriptorHeaps(1, Heaps);

        auto Barrier = [&](ID3D12Resource* Res, D3D12_RESOURCE_STATES Before,
                           D3D12_RESOURCE_STATES After) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = Res;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = Before;
            B.Transition.StateAfter  = After;
            CL->ResourceBarrier(1, &B);
        };
        ID3D12Resource* const Resources[3] = { IrradTex.Get(), DistTex.Get(), ProbeBuf.Get() };
        const float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (u32 i = 0; i < 3; ++i) {
            Barrier(Resources[i], D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(UavBase + i),
                                              _SRVHeap.CpuHandleStaging(UavBase + i),
                                              Resources[i], Zero, 0, nullptr);
            Barrier(Resources[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
        }
        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL };
        _Queue.ExecuteAndSync(Lists, 1);

        // Depois do sync: os descritores de UAV ja cumpriram o papel e a GPU terminou de usa-los.
        _SRVHeap.Free(UavBase, 3);
        Ready = true;
    }

    void FGIFallbackResources::Release(FTextureSRVHeap& _SRVHeap) {
        auto Free = [&](u32& Slot) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, 1); Slot = kInvalidSlot; }
        };
        Free(IrradSRV);
        Free(DistSRV);
        Free(ProbeDataSRV);
        IrradTex.Reset();
        DistTex.Reset();
        ProbeBuf.Reset();
        Ready = false;
    }

    FGIFallbackBindings FGIFallbackResources::Bindings() const {
        FGIFallbackBindings B{};
        B.IrradianceAtlasSRV = IrradSRV;
        B.DistanceAtlasSRV   = DistSRV;
        B.ProbeDataSRV       = ProbeDataSRV;
        B.Available          = false; // neutro E "sem volume", por definicao
        return B;
    }
}
