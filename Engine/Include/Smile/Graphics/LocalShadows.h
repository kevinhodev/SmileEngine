#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;
    class FGpuMesh;
    class FMaterial;

    // Sombras das luzes LOCAIS. Modelado no FSunShadows; mesmos shaders de depth do CSM
    // (ShadowDepth.vs/ps: alpha-test de folhagem de graca). Dois caminhos:
    //  - SPOT (F3a): Texture2DArray D32 1024^2, um slice por luz (budget kMaxShadows/frame),
    //    projecao fov = 2*coneExterno / far = raio; o deferred lighting le t18 e faz PCF 3x3
    //    com a matriz world->UVZ que viaja no FGPULight (slice em SpotParams.y; -1 = sem).
    //  - POINT (F3b): TextureCubeArray D32 512^2 x kMaxCubeShadows (6 faces de 90 graus por
    //    luz, convencao de faces D3D). O lookup nao usa matriz: o vetor luz->pixel escolhe a
    //    face no hardware e a profundidade de referencia sai do EIXO DOMINANTE (mesma
    //    projecao das faces) — t19, indice do cubo em SpotParams.y.
    class FLocalShadows {
    public:
        static constexpr u32 kMaxShadows     = 8;
        static constexpr u32 kResolution     = 1024;
        static constexpr u32 kMaxCubeShadows = 4;
        static constexpr u32 kCubeResolution = 512;
        static constexpr f32 kPointNear      = 0.05f; // near de TODAS as projecoes locais (spot e
                                                      // faces do cubo) — o shader reconstroi o refZ
                                                      // linear com ele (LightParams2.y)

        struct FShadowDrawItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Mat;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCB;
            Vec3                      AABBMin;
            Vec3                      AABBMax;
        };

        // Um slice de SPOT a renderizar neste frame: matriz da luz + esfera de influencia
        // (cull de casters — AABB vs esfera; o cone exato fica pra depois, a esfera corta bem).
        struct FShadowJob {
            Mat44 ViewProj;
            Vec3  LightPos;
            f32   Radius;
            u32   Slice;
        };

        // Um cubo de POINT a renderizar (6 faces): so posicao + raio; as matrizes das faces
        // sao fixas por convencao (D3D cube faces) e construidas aqui dentro.
        struct FCubeShadowJob {
            Vec3 LightPos;
            f32  Radius;
            u32  CubeIndex;
        };

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        bool IsInitialized() const { return Initialized; }

        void RecordDepthPass(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                             u32 FrameSlot, const FShadowDrawItem* Items, size_t Count,
                             const FShadowJob* Jobs, u32 JobCount,
                             const FCubeShadowJob* CubeJobs, u32 CubeJobCount);

        // Garante os arrays legiveis (PIXEL_SHADER_RESOURCE) mesmo em frame sem job novo.
        void EnsureReadable(ID3D12GraphicsCommandList* CommandList);

        // Slot do SRV do atlas de spot (t18); o SRV do cube array (t19) vive em Slot+1 —
        // a tabela do root param cobre os dois de uma vez (descritores contiguos).
        u32 ShadowSRVSlot() const { return ShadowSRVSlot_; }

        f32  GetDepthBias() const { return DepthBias; }
        void SetDepthBias(f32 B)  { DepthBias = B; }

    private:
        void CreateResources(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);
        void TransitionArray(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);
        void TransitionCube(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);

        Microsoft::WRL::ComPtr<ID3D12Resource>      DepthArray;
        FDescriptorHeap                             DSVHeap;
        u32                                         ShadowSRVSlot_ = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES                       ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12Resource>      CubeArray;   // 6*kMaxCubeShadows slices
        FDescriptorHeap                             CubeDSVHeap;
        D3D12_RESOURCE_STATES                       CubeState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OpaquePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskedPSO;

        Microsoft::WRL::ComPtr<ID3D12Resource>      SliceCB; // Mat44 por slice por frame em voo
        u8*                                         MappedSlice = nullptr;

        f32  DepthBias   = 0.02f; // bias constante em METROS (o shader converte pra NDC pelo
                                  // caminho linear; + termo relativo por distancia no shader).
                                  // Bias NDC constante em perspectiva = peter-panning na borda.
        bool Initialized = false;
    };
}
