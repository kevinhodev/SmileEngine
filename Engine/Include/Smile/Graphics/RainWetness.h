#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Weather.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FGBuffer;

    struct alignas(256) RainWetnessConstants {
        Mat44 InvViewProj;   // inversa FULL da view-proj jitterada (reconstrucao do worldPos,
                             // mesma conta do DeferredLighting)
        Vec4  CameraWorldPos;// xyz = camera (mundo), w = tempo (s) — anima os ripples
        Vec4  RainParams0;   // x = RainAmount, y = PuddleAmount, z = RippleStrength,
                             // w = WetDarkening
        Vec4  RainParams1;   // x = 1/PuddleScale (1/m), yzw = -
    };

    // Chuva deferred — F1 (padrao CRainStage::ExecuteDeferredRainGBuffer da CryEngine, com
    // mascaras PROCEDURAIS no lugar das texturas de asset): copia GBufferA/B pra scratch e
    // reescreve os originais num fullscreen pass — albedo escurecido por porosidade, roughness
    // baixado, pocas up-facing com normal achatada + aneis de gota analiticos. Roda logo apos
    // o geometry pass, entao TODOS os consumidores do G-buffer (lighting, reflexoes RT, ReSTIR)
    // veem a cena ja molhada — rua espelhada nas reflexoes vem de graca.
    class FRainWetness {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Recreate(ID3D12Device* Device); // reload de shader (PSO apenas)

        bool IsInitialized() const { return PSO != nullptr; }

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProjFull,
                            const Vec3& CameraWorldPos, f32 TimeSec, const FWeather& Weather);

        // Pre-condicoes (estado no ponto do frame em que roda): G-buffer em RENDER_TARGET
        // (saida do geometry pass) e depth em DEPTH_WRITE — restaura os dois ao final.
        void Execute(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                     FGBuffer& GBuffer, ID3D12Resource* DepthBuffer, u32 DepthSRVSlot,
                     u32 Width, u32 Height);

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device);
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateScratch(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;

        // Copias de leitura de GBufferA/B (mesmos formatos), SRVs contiguos [A,B] p/ uma tabela.
        Microsoft::WRL::ComPtr<ID3D12Resource> ScratchA;
        Microsoft::WRL::ComPtr<ID3D12Resource> ScratchB;
        u32 ScratchSRVBase = 0xFFFFFFFFu;
        u32 ScratchW = 0, ScratchH = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8* MappedBase = nullptr;
        u32 FrameSlot  = 0;

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<u64>(FrameSlot) * sizeof(RainWetnessConstants);
        }
    };
}
