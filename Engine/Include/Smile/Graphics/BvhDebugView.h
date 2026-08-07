#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Visualizacao de debug da BVH — secao 7.3.3 do GPU Zen 3 (cap. 7, Cyberpunk 2077). Traca um
    // raio primario por pixel direto na TLAS, num dispatch proprio, e publica o resultado no
    // DebugTargets. Existe porque num renderer hibrido NADA na tela mostra o conteudo da TLAS: os
    // raios primarios sao rasterizados, e os de RT sao secundarios e ja partem do G-buffer.
    //
    // Nao concorre com o FShaderTimer: aquele mede CICLOS dentro dos passes reais de RT e depende
    // da NVAPI; este e portatil e responde "o que esta na estrutura e quao densa ela e".
    //
    // Passe de DEBUG: so roda com o toggle ligado (Renderer::SetBvhDebug), e desligado nao aloca
    // nem dispara nada.
    class FBvhDebugView : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Debug da BVH"; }
        bool IsInitialized() const override { return Target != nullptr && SceneTableReady; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        // Espelha o modo lido de BvhDebugParams.x em Shaders/Debug/BvhDebug.cs.hlsl.
        enum class EMode : u32 {
            Category = 0,  // cor por categoria da instancia na TLAS (opaco/alpha-test/translucido)
            Instance,      // cor por hash do InstanceID (acha duplicata e sobreposicao)
            Complexity,    // triangulos testados ao longo do raio, em falsa-cor
            Count
        };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        // Teto default do heatmap de complexidade, em triangulos testados por raio. Escolhido p/
        // a Bistro: o miolo da rua fica no meio da rampa e so folhagem empilhada satura.
        static constexpr f32 kDefaultComplexityMax = 96.0f;

        void Initialize(ID3D12Device* Device);
        // Recria alvo, views e a tabela de UAV. Chamado no resize da tela.
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Release(FTextureSRVHeap& SRVHeap);

        // Liga a TLAS e o snapshot de instancias (FDDGI::InstanceSRV). Precisa ser refeito toda
        // vez que a cena de RT e reconstruida — os slots mudam. Sem os dois, o passe nao roda.
        void SetSceneSRVs(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                          u32 TlasSRVSlot, u32 InstanceSRVSlot);

        bool IsReady() const { return Target != nullptr && SceneTableReady; }
        u32  SrvSlot() const { return SRVSlot; }

        void Render(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                    const Mat44& InvViewProj, const Vec3& CameraPos,
                    EMode Mode, f32 MaxRayDist, f32 ComplexityMax, u32 FrameSlot);
        // Deixa legivel pelo visualizador (passe grafico).
        void ToRead(ID3D12GraphicsCommandList* CL);

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        struct alignas(256) BvhDebugConstants {
            Mat44 InvViewProj;
            Vec4  CameraPos;
            Vec4  ScreenParams;
            Vec4  Params;      // x = modo, y = alcance, z = teto do heatmap, w = -
        };

        void CreateConstantBuffer(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, D3D12_RESOURCE_STATES After);

        FVolumetricPipeline PSO; // 2 SRV [TLAS, InstanceGeo], 1 UAV; heap diretamente indexado
                                 // (VB/IB do alpha-test e da normal vem bindless pelo InstanceGeo)

        Microsoft::WRL::ComPtr<ID3D12Resource> Target;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
        u32 SRVSlot  = kInvalidSlot;
        u32 UAVSlot  = kInvalidSlot;
        u32 UAVTable = kInvalidSlot;

        // Tabela t0/t1 contigua. Uma so, sem versao por frame em voo: os dois descritores mudam
        // apenas quando a cena de RT e reconstruida, e ali a GPU ja esta ociosa.
        u32  SceneTable      = kInvalidSlot;
        bool SceneTableReady = false;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;

        u32 Width_ = 0, Height_ = 0;
        bool Initialized = false;
    };
}
