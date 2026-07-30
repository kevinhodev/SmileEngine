#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/CommandQueue.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Constantes do DI-lite (b0). Casa campo-a-campo com o DILiteCB de Lighting/DILite.cs.hlsl.
    struct alignas(256) DILiteConstants {
        Mat44 InvViewProj;   // FULL (jittered) — reconstroi worldPos do depth
        Vec4  CameraPos;     // xyz = camera em mundo
        Vec4  ScreenParams;  // W, H, 1/W, 1/H
        Vec4  Params;        // x = nº de luzes, y = frameIndex, z = mascara do shadow ray,
                             // w = margem final do raio (m)
        Vec4  RayEpsA;       // perfil compartilhado (contrato do RayOffset.hlsli)
        Vec4  RayEpsB;
    };

    // DI-lite — direta das luzes locais que ficaram SEM slot de shadow map, com UM raio de sombra
    // por PIXEL (nao por luz): todas as elegiveis entram num stream de WRS, uma vence, e o
    // estimador RIS devolve sem vies a contribuicao do conjunto. Degrau para o ReSTIR DI — falta
    // reuso temporal e espacial por cima deste reservoir de uma amostra.
    //
    // A divisao de trabalho com o deferred e a fracao SpotParams.w de cada luz (ver FGPULight):
    // o deferred fica com (1-w), este passe com w. Sem DXR nao roda (IsReady() = false) e o
    // deferred volta a somar a luz inteira sozinho — degeneracao automatica, sem knob.
    //
    // O sinal de saida e RUIDOSO (1 raio/pixel de visibilidade). Sob Ray Reconstruction e o que a
    // rede espera receber; sob NRD ele NAO passa por denoiser (o NRD trata GI e reflexao, nao este
    // alvo), e por isso o default no Renderer nasce OFF.
    class FDILite {
    public:
        void Initialize(ID3D12Device* Device);

        // (Re)cria o alvo e monta as tabelas. Uma tabela por frame em voo: o unico descritor que
        // varia entre elas e o das luzes (t6), cujo SRV descreve a fatia daquele frame. Chamar no
        // setup da cena e em TODO resize (depth/gbuffer recriados).
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height,
                            u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot, u32 DepthSlot,
                            u32 TlasSlot, u32 InstanceSlot,
                            const u32 LightSlots[FCommandQueue::kFramesInFlight]);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Vec3& CameraPos,
                            u32 Width, u32 Height, u32 FrameIndex, u32 LightCount,
                            u32 ShadowRayMask);

        void RecordDispatch(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Deixa o alvo legivel pelo deferred (que o soma). Separado do dispatch porque o Renderer
        // agrupa barreiras.
        void TransitionForRead(ID3D12GraphicsCommandList* CL);

        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }

        bool IsReady() const        { return Ready; }
        u32  OutputSRVSlot() const  { return OutSRV; }

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FVolumetricPipeline PSO; // 7 SRV, 1 UAV, heap-directly-indexed (bindless do alpha-test)

        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_COMMON;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 OutSRV = kInvalidSlot;
        u32 OutUAV = kInvalidSlot;
        u32 Table[FCommandQueue::kFramesInFlight] = {};

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB  = nullptr;
        u32 FrameSlot = 0;
        DILiteConstants CPU{};

        u32  Width = 0, Height = 0;
        bool Initialized = false;
        bool Ready       = false;

        FRayEpsilonProfile RayEps; // dono = Renderer, empurrado todo frame
    };
}
