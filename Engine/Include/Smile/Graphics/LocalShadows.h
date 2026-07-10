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

    // Sombras das luzes LOCAIS (F3a: spot; point cubemap vem depois). Modelado no FSunShadows:
    // Texture2DArray D32 com um slice por luz sombreada (budget kMaxShadows/frame — o Renderer
    // escolhe os spots mais proximos da camera), mesmos shaders de depth do CSM (ShadowDepth.vs/
    // ps: alpha-test de folhagem de graca), projecao perspectiva fov = 2*coneExterno e far =
    // raio de atenuacao. O deferred lighting le o array em t18 e faz PCF 3x3 com a matriz
    // world->UVZ que viaja DENTRO do FGPULight (slice em SpotParams.y; -1 = sem sombra).
    class FLocalShadows {
    public:
        static constexpr u32 kMaxShadows = 8;
        static constexpr u32 kResolution = 1024;

        struct FShadowDrawItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Mat;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCB;
            Vec3                      AABBMin;
            Vec3                      AABBMax;
        };

        // Um slice a renderizar neste frame: matriz da luz + esfera de influencia (cull de
        // casters — AABB vs esfera; o cone exato fica pra depois, a esfera ja corta bem).
        struct FShadowJob {
            Mat44 ViewProj;
            Vec3  LightPos;
            f32   Radius;
            u32   Slice;
        };

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        bool IsInitialized() const { return Initialized; }

        void RecordDepthPass(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                             u32 FrameSlot, const FShadowDrawItem* Items, size_t Count,
                             const FShadowJob* Jobs, u32 JobCount);

        // Garante o array legivel (PIXEL_SHADER_RESOURCE) mesmo em frame sem job novo.
        void EnsureReadable(ID3D12GraphicsCommandList* CommandList);

        u32 ShadowSRVSlot() const { return ShadowSRVSlot_; }

        f32  GetDepthBias() const { return DepthBias; }
        void SetDepthBias(f32 B)  { DepthBias = B; }

    private:
        void CreateResources(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);
        void TransitionArray(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);

        Microsoft::WRL::ComPtr<ID3D12Resource>      DepthArray;
        FDescriptorHeap                             DSVHeap;
        u32                                         ShadowSRVSlot_ = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES                       ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OpaquePSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskedPSO;

        Microsoft::WRL::ComPtr<ID3D12Resource>      SliceCB; // Mat44 por slice por frame em voo
        u8*                                         MappedSlice = nullptr;

        f32  DepthBias   = 0.0015f; // bias constante em NDC z (soma ao slope-scaled do PSO)
        bool Initialized = false;
    };
}
