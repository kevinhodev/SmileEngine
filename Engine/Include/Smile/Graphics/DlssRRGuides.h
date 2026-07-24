#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Produz os guides de material que o DLSS Ray Reconstruction consome (ver FDlssRRPass +
    // Shaders/Upscale/DlssRRGuides|SpecHit.cs.hlsl). Todos render-res, lineares:
    //   DiffuseAlbedo / SpecularAlbedo / NormalRoughness (RGBA16F) derivados do G-buffer;
    //   SpecHitDist (R16F) extraido do Resolved da reflexao (distancia world-space do raio).
    // O RR taga esses recursos DIRETO (ID3D12Resource* — NGX cria os descritores internamente), entao
    // a classe so precisa dos UAVs de escrita: nao aloca SRVs. Os estados sao rastreados aqui; o
    // Renderer chama RecordGuides (+ RecordSpecHitDist/ClearSpecHitDist) e depois TransitionForRR
    // antes do slEvaluateFeature(kFeatureDLSS_RR); o RR nao muda o estado dos guides.
    class FDlssRRGuides {
    public:
        void Initialize(ID3D12Device* Device);   // 2 PSOs compute
        void Shutdown();
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        bool IsReady() const { return Ready; }

        // Deriva diffuse/specular albedo + normal-roughness do G-buffer. GBufferTable = t0-t3
        // [A,B,C,Depth] (SRVHeap.GpuHandle(FGBuffer::SRVTableStart())). FrameCB = CBV b0 do frame
        // (FrameConstants). Caller ja transicionou o G-buffer/Depth p/ legiveis por shader. Deixa os
        // 3 guides em UNORDERED_ACCESS.
        void RecordGuides(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                          D3D12_GPU_DESCRIPTOR_HANDLE GBufferTable, D3D12_GPU_VIRTUAL_ADDRESS FrameCB);

        // Extrai o specHitDist do Resolved da reflexao. ResolvedSrv = SRVHeap.GpuHandle(slot do
        // Resolved). Caller garante o Resolved legivel por shader (NON_PIXEL). Deixa SpecHitDist em UAV.
        void RecordSpecHitDist(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                               D3D12_GPU_DESCRIPTOR_HANDLE ResolvedSrv);

        // Sem reflexoes ativas: zera o specHitDist (evita lixo indefinido entrando no RR). Deixa em UAV.
        void ClearSpecHitDist(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Transiciona os 4 guides p/ NON_PIXEL_SHADER_RESOURCE (estado declarado nas tags do RR).
        void TransitionForRR(ID3D12GraphicsCommandList* CL);

        ID3D12Resource* DiffuseAlbedo()   const { return DiffAlb.Get(); }
        ID3D12Resource* SpecularAlbedo()  const { return SpecAlb.Get(); }
        ID3D12Resource* NormalRoughness() const { return NrmRough.Get(); }
        ID3D12Resource* SpecHitDist()     const { return SpecHit.Get(); }

    private:
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        void ReleaseResize(FTextureSRVHeap& SRVHeap);

        FVolumetricPipeline GuidesPSO;   // 4 SRV [A,B,C,Depth], 3 UAV [diffAlb, specAlb, normalRough]
        FVolumetricPipeline SpecHitPSO;  // 1 SRV [Resolved],     1 UAV [specHitDist]

        Microsoft::WRL::ComPtr<ID3D12Resource> DiffAlb, SpecAlb, NrmRough, SpecHit;
        D3D12_RESOURCE_STATES DiffAlbState   = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES SpecAlbState   = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES NrmRoughState  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES SpecHitState   = D3D12_RESOURCE_STATE_COMMON;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 MainUavTable = kInvalidSlot; // 3 UAVs contiguos [diffAlb, specAlb, normalRough]
        u32 SpecHitUav   = kInvalidSlot; // UAV do specHitDist (tabela 1-wide + clear)
        u32 Width = 0, Height = 0;
        bool Initialized = false;
        bool Ready       = false;
    };
}
