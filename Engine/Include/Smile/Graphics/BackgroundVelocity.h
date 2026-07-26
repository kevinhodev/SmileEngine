#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Preenche o motion vector do BACKGROUND (ceu/nuvens/fog compostos no HDR sem geometria) que o G-buffer
    // deixa ZERO. Sem isso, DLSS/RR/TAA tratam o ceu como "parado" e reprojetam o historico do lugar errado
    // ao girar a camera => ghosting (pior subindo a camera). Passe compute dedicado (1 SRV depth, 1 UAV
    // velocity), rodado APOS o G-buffer, escrevendo curUV-prevUV so nos pixels de background (reverse-Z:
    // deviceZ<=0). Ver Shaders/Upscale/BackgroundVelocity.cs.hlsl.
    class FBackgroundVelocity {
    public:
        void Initialize(ID3D12Device* Device);   // 1 PSO compute + ring de constantes (1 matriz)
        void Shutdown();
        bool IsReady() const { return Initialized; }

        // O caller garante ANTES da chamada: Depth em NON_PIXEL_SHADER_RESOURCE, Velocity em
        // UNORDERED_ACCESS, e o heap de descritores da cena ligado. DepthSrv = t0 (SRV do depth),
        // VelocityUav = u0 (UAV do velocity). SkyClipToPrevClip = clip atual -> clip anterior SEM
        // translacao (reprojecao do ceu no infinito).
        void Record(ID3D12GraphicsCommandList* CL, u32 FrameSlot,
                    D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv, D3D12_GPU_DESCRIPTOR_HANDLE VelocityUav,
                    const Mat44& SkyClipToPrevClip, u32 Width, u32 Height);

    private:
        FVolumetricPipeline PSO;                           // 1 SRV [Depth], 1 UAV [Velocity]
        Microsoft::WRL::ComPtr<ID3D12Resource> Constants;  // ring kFramesInFlight * 256B (SkyClipToPrevClip)
        u8*  MappedConstants = nullptr;
        bool Initialized     = false;
    };
}
