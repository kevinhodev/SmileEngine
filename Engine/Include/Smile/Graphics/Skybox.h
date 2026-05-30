#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Mat44.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    // Draws an environment cubemap as the scene background. Uses a fullscreen
    // triangle with depth=1 and DepthFunc=LESS_EQUAL so it renders at the far
    // plane behind every actual scene pixel.
    class FSkybox {
    public:
        // SampleCount must match the active scene RT (MSAA).
        void Initialize(ID3D12Device* Device, u32 SampleCount,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Recreate(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        // Records the fullscreen draw. Caller must already have set viewport,
        // scissor, render target, depth target, and descriptor heaps. Binds the
        // env cube SRV from the active descriptor heap at EnvCubeSRVSlot.
        void Render(ID3D12GraphicsCommandList* CommandList,
                    FTextureSRVHeap& SRVHeap,
                    u32 EnvCubeSRVSlot,
                    const Mat44& InvViewProjNoTranslation,
                    f32 IBLIntensity, f32 IBLRotation);

    private:
        struct alignas(256) SkyboxConstants {
            Mat44 InvViewProjNoTranslation; // 64
            f32   IBLIntensity;             // 4
            f32   IBLRotation;              // 4
            f32   _Pad0;                    // 4
            f32   _Pad1;                    // 4
            u8    _Tail[256 - 80] = {};
        };
        static_assert(sizeof(SkyboxConstants) == 256, "SkyboxConstants must be 256 bytes");

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> CBV;
        SkyboxConstants* MappedCBV = nullptr;
    };
}
