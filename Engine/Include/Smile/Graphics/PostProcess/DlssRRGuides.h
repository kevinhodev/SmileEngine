#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/Backend/D3D12/DescriptorHeap.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
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
    class FDlssRRGuides : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "DLSS-RR guides"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        void Initialize(ID3D12Device* Device);   // 2 PSOs compute
        void RecreatePSOs(ID3D12Device* Device);
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
        // FrameCB so existe p/ preencher o b0 que o root sig do FComputePipeline declara — o shader
        // nao le nada dele (root param nao referenciado e legal em D3D12, mas o GBV reclama).
        void RecordSpecHitDist(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                               D3D12_GPU_DESCRIPTOR_HANDLE ResolvedSrv,
                               D3D12_GPU_VIRTUAL_ADDRESS FrameCB);
        // Sobrescreve somente pixels de agua, preservando o hit dos opacos fora dela.
        void RecordWaterSpecHitDist(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                                    D3D12_GPU_DESCRIPTOR_HANDLE WaterTable,
                                    D3D12_GPU_VIRTUAL_ADDRESS FrameCB);

        // Sem reflexoes ativas: zera o specHitDist (evita lixo indefinido entrando no RR). Deixa em UAV.
        void ClearSpecHitDist(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // A agua e composta depois da reflexao. Antes do draw, torna o spec-hit um RTV para a
        // superficie zerar apenas os pixels que substituem o opaco que originou aquele hit.
        void PrepareSpecHitForWater(ID3D12GraphicsCommandList* CL);
        D3D12_CPU_DESCRIPTOR_HANDLE SpecHitRTV() const { return SpecHitRTVHeap.CpuHandle(0); }

        // Transiciona os 4 guides p/ NON_PIXEL_SHADER_RESOURCE (estado declarado nas tags do RR).
        void TransitionForRR(ID3D12GraphicsCommandList* CL);

        // Deixa os 4 legiveis por um passe GRAFICO (o visualizador de debug). Existe separado do
        // TransitionForRR porque as tags do RR declaram NON_PIXEL puro (DlssRRPass.cpp) e o
        // estado combinado nao pode vazar para elas. Nao precisa de "restore": o proprio
        // TransitionForRR, que roda depois no mesmo frame, devolve tudo a NON_PIXEL — os estados
        // sao rastreados aqui dentro. O FDebugView nao emite barreira por conta propria, entao
        // sem isto o alvo seria lido em estado errado (mesma armadilha do ReSTIR GI).
        void TransitionForDebug(ID3D12GraphicsCommandList* CL);

        ID3D12Resource* DiffuseAlbedo()   const { return DiffAlb.Get(); }
        ID3D12Resource* SpecularAlbedo()  const { return SpecAlb.Get(); }
        ID3D12Resource* NormalRoughness() const { return NrmRough.Get(); }
        ID3D12Resource* SpecHitDist()     const { return SpecHit.Get(); }

        // SRVs SO para o visualizador. O RR taga os ID3D12Resource* direto (a NGX cria os
        // descritores dela), entao estes quatro nao participam do caminho de render — existem
        // porque os guides eram os unicos sinais grandes da engine sem inspecao, e isso custou
        // uma noite de bisect as cegas em 2026-08-07.
        u32 DiffuseAlbedoSRV()   const { return GuideSrvBase == kInvalidSlot ? kInvalidSlot : GuideSrvBase + 0; }
        u32 SpecularAlbedoSRV()  const { return GuideSrvBase == kInvalidSlot ? kInvalidSlot : GuideSrvBase + 1; }
        u32 NormalRoughnessSRV() const { return GuideSrvBase == kInvalidSlot ? kInvalidSlot : GuideSrvBase + 2; }
        u32 SpecHitDistSRV()     const { return GuideSrvBase == kInvalidSlot ? kInvalidSlot : GuideSrvBase + 3; }

    private:
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        void ReleaseResize(FTextureSRVHeap& SRVHeap);

        FComputePipeline GuidesPSO;   // 4 SRV [A,B,C,Depth], 3 UAV [diffAlb, specAlb, normalRough]
        FComputePipeline SpecHitPSO;  // 1 SRV [Resolved],     1 UAV [specHitDist]
        FComputePipeline WaterSpecHitPSO; // 2 SRV [waterResolved, GBufferB], 1 UAV

        Microsoft::WRL::ComPtr<ID3D12Resource> DiffAlb, SpecAlb, NrmRough, SpecHit;
        FDescriptorHeap SpecHitRTVHeap;
        D3D12_RESOURCE_STATES DiffAlbState   = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES SpecAlbState   = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES NrmRoughState  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES SpecHitState   = D3D12_RESOURCE_STATE_COMMON;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 MainUavTable = kInvalidSlot; // 3 UAVs contiguos [diffAlb, specAlb, normalRough]
        u32 SpecHitUav   = kInvalidSlot; // UAV do specHitDist (tabela 1-wide + clear)
        u32 GuideSrvBase = kInvalidSlot; // 4 SRVs contiguos [diffAlb, specAlb, nrmRough, specHit]
        u32 Width = 0, Height = 0;
        bool Initialized = false;
        bool Ready       = false;
    };
}
