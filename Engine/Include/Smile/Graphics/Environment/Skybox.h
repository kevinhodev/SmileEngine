#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Mat44.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FSkybox : public FRenderPass {
    public:
        const char* Name() const override { return "Céu (skybox HDRI)"; }
        bool IsInitialized() const override { return PSO != nullptr; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        void Initialize(ID3D12Device* Device,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Recreate(ID3D12Device* Device,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        void Render(u32 FrameSlot,
                    ID3D12GraphicsCommandList* CommandList,
                    FTextureSRVHeap& SRVHeap,
                    u32 EnvCubeSRVSlot,
                    const Mat44& InvViewProjNoTranslation,
                    f32 IBLIntensity, f32 IBLRotation);

    private:
        struct alignas(256) SkyboxConstants {
            Mat44 InvViewProjNoTranslation; 
            f32   IBLIntensity;             
            f32   IBLRotation;              
            f32   _Pad0;                    
            f32   _Pad1;                    
            u8    _Tail[256 - 80] = {};
        };
        static_assert(sizeof(SkyboxConstants) == 256, "SkyboxConstants must be 256 bytes");

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> CBV;
        u8*              MappedCBVBase = nullptr;
        SkyboxConstants* MappedCBV     = nullptr;
    };
}
