#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/Weather.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FGBuffer;
    class FGpuMesh;
    class FMaterial;

    struct alignas(256) RainWetnessConstants {
        Mat44 InvViewProj;   // inversa FULL da view-proj jitterada (reconstrucao do worldPos,
                             // mesma conta do DeferredLighting)
        Vec4  CameraWorldPos;// xyz = camera (mundo), w = tempo (s) — anima os ripples
        Vec4  RainParams0;   // x = RainAmount, y = PuddleAmount, z = RippleStrength,
                             // w = WetDarkening
        Vec4  RainParams1;   // x = 1/PuddleScale (1/m), yzw = -
        Mat44 RainOccMatrix; // F2: world -> UVZ do mapa de oclusao (ortho top-down + bias UV)
        Vec4  RainOccParams; // x = enabled (0/1), y = bias (depth), z = 1/banda suave (depth),
                             // w = resolucao do mapa
        Vec4  CurtainParams; // F3: x = CurtainAmount x rain, y = queda (m/s),
                             // F5: z = particulas ativas (cortina poupa o near), w = -
        Vec4  KeyLightDir;   // F3: xyz = dir PARA a key light (mundo)
        Vec4  KeyLightColor; // F3: rgb = cor x intensidade da key light
        Vec4  SkyAmbientRain;// F3: rgb = ambient fisico do ceu (streaks visiveis de costas
                             //      pra luz e a noite)
        Mat44 RainViewProj;  // F5: view-proj FULL jitterada (VS das particulas projeta mundo)
        Vec4  RainParticleParams; // F5: x = raio XZ do box (m), y = altura (m),
                                  //     z = -, w = num de particulas
    };

    // Chuva deferred — F1 (padrao CRainStage::ExecuteDeferredRainGBuffer da CryEngine, com
    // mascaras PROCEDURAIS no lugar das texturas de asset): copia GBufferA/B pra scratch e
    // reescreve os originais num fullscreen pass — albedo escurecido por porosidade, roughness
    // baixado, pocas up-facing com normal achatada + aneis de gota analiticos. Roda logo apos
    // o geometry pass, entao TODOS os consumidores do G-buffer (lighting, reflexoes RT, ReSTIR)
    // veem a cena ja molhada — rua espelhada nas reflexoes vem de graca.
    class FRainWetness : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Chuva"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        // F2: occluder da chuva (mesmos campos do FShadowDrawItem do CSM — o Renderer monta
        // a lista do mesmo AllItems; vidro ENTRA: telhado de vidro bloqueia chuva).
        struct FOccluderItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Mat;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCB;
            Vec3                      AABBMin;
            Vec3                      AABBMax;
        };

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void Recreate(ID3D12Device* Device); // reload de shader (PSO apenas)

        bool IsInitialized() const override { return PSO != nullptr; }

        // F2: renderiza o mapa de oclusao top-down se preciso (centro snapado mudou ou cache
        // invalido). Retorna true se re-renderizou (o caller restaura o estado de cena).
        // Cacheado estilo Cry (bProcessed): andar so re-renderiza ao cruzar o quantum de snap.
        bool RecordOcclusionMap(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                                u32 FrameSlot, const Vec3& CamPos,
                                const FOccluderItem* Items, size_t Count);
        void InvalidateOcclusion() { OccValid = false; }

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProjFull,
                            const Mat44& ViewProjFull, const Vec3& CameraWorldPos, f32 TimeSec,
                            const FWeather& Weather, const Vec3& KeyDir,
                            const Vec3& KeyColorTimesInt, const Vec3& SkyAmbient);

        // Pre-condicoes (estado no ponto do frame em que roda): G-buffer em RENDER_TARGET
        // (saida do geometry pass) e depth em DEPTH_WRITE — restaura os dois ao final.
        void Execute(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                     FGBuffer& GBuffer, ID3D12Resource* DepthBuffer, u32 DepthSRVSlot,
                     u32 Width, u32 Height);

        // F3: cortina de gotas (estilo SceneRain da Cry, cilindros conceituais no fullscreen
        // pass — 3 raios com parallax, streaks procedurais, tint da key light, soft-depth e
        // oclusao pelo mapa da F2). Blend premultiplicado por cima do HDR ja com fog; o
        // caller poe o RTV do HDR e o depth como SRV (mesmo padrao do Fog.Execute).
        void ExecuteCurtain(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                            u32 DepthSRVSlot, u32 Width, u32 Height);

        // F5a: gotas por particula — DrawInstanced de quads 100% procedurais (hash + tempo,
        // box com wrap preso na camera, colisao pelo occlusion map como heightmap no VS).
        // Mesmas pre-condicoes do ExecuteCurtain; chamar logo depois dele.
        void ExecuteParticles(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& SRVHeap,
                              u32 DepthSRVSlot, u32 Width, u32 Height);

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device);
        void BuildCurtainPSO(ID3D12Device* Device);   // F3 (mesmo root sig da wetness)
        void BuildParticlesPSO(ID3D12Device* Device); // F5 (idem)

        static constexpr u32 kMaxRainParticles = 32768; // box 24x24x24 m ~ 2.4/m3 no maximo
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateScratch(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void BuildOcclusion(ID3D12Device* Device, FTextureSRVHeap& SRVHeap); // F2: mapa + PSOs

        // ---- F2: mapa de oclusao (depth ortho top-down 512^2 ao redor da camera) ----
        static constexpr u32 kOccSize       = 512;
        static constexpr f32 kOccHalfExtent = 60.0f;  // cobre 120 m ao redor da camera
        static constexpr f32 kOccSnap       = 16.0f;  // quantum do centro: re-render ao cruzar
        static constexpr f32 kOccUp         = 150.0f; // teto acima da camera (telhados ocluem)
        static constexpr f32 kOccDown       = 50.0f;  // piso abaixo (camera em mezanino/ponte)

        Microsoft::WRL::ComPtr<ID3D12RootSignature> OccRootSig; // clone do layout do CSM
                                                                // (Material::Bind assume 1/2)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OccOpaquePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OccMaskedPSO;
        Microsoft::WRL::ComPtr<ID3D12Resource>      OccDepth;
        FDescriptorHeap                             OccDSVHeap;
        u32                                         OccSRVSlot = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES OccState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        Microsoft::WRL::ComPtr<ID3D12Resource>      OccCB; // LightViewProj por frame em voo
        u8*                                         MappedOccCB = nullptr;
        Mat44 OccWorldToUVZ{};       // matriz do ULTIMO render (o CB da wetness usa esta,
                                     // nao a do frame corrente — o mapa e cacheado)
        Vec3  OccSnapped{};          // centro snapado do ultimo render
        bool  OccValid        = false;
        bool  OccEverRendered = false;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CurtainPSO;   // F3
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ParticlesPSO; // F5a
        Microsoft::WRL::ComPtr<ID3D12PipelineState> SplashPSO;    // F5b
        u32 ParticleCount = 0; // instancias deste frame (escala com a chuva; 0 = nao desenha)

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
