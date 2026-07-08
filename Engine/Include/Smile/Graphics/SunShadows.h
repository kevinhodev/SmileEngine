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

    struct alignas(256) CSMConstants {
        Mat44 WorldToShadow[4];  // 4*64 = 256 bytes
        Vec4  CascadeTexelWorld; // x..w = tamanho de 1 texel em mundo, por cascata (normal-offset)
        Vec4  Params;            // x = numCascades, y = depthBias (NDC z), z = 1/res, w = enabled
        Vec4  Params2;           // x = normal-offset (em texels), yzw reservado
        Vec4  Params3;           // x = frame do ruido do PCF (0 = estatico, sem TAA/FSR2)
    };

    struct alignas(256) ShadowCascadeConstants {
        Mat44 LightViewProj;
    };

    class FSunShadows {
    public:
        static constexpr u32 kNumCascades = 4;
        static constexpr u32 kResolution  = 2048;

        struct FShadowDrawItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Mat;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCB;
            Vec3                      AABBMin;
            Vec3                      AABBMax;
        };

        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        void UpdatePerFrame(u32 FrameSlot, bool Enabled, const Mat44& View, const Vec3& CamPos,
                            f32 FovYRadians, f32 Aspect, const Vec3& DirToSun, f32 NearZ,
                            f32 NoiseFrame);

        void RecordDepthPass(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                             const FShadowDrawItem* Items, size_t Count);

        void EnsureReadable(ID3D12GraphicsCommandList* CommandList);

        u32  ShadowSRVSlot() const { return ShadowSRVSlot_; }
        D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress() const {
            return CSMCB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(CSMConstants);
        }
        bool IsInitialized() const { return Initialized; }

        void SetMaxDistance(f32 D)      { ShadowMaxDistance = D; }
        void SetDepthBias(f32 B)        { DepthBias = B; }
        void SetCasterPullback(f32 P)   { CasterPullback = P; }
        void SetNormalOffset(f32 Texels){ NormalOffsetTexels = Texels; } 
        void SetPenumbra(f32 Texels)    { PcfRadiusTexels = Texels; }    
        void SetBlendBand(f32 UV)       { BlendBand = UV; }              
        void SetDebugCascades(bool On)  { DebugCascades = On; }          
        f32  GetMaxDistance() const     { return ShadowMaxDistance; }
        f32  GetDepthBias() const       { return DepthBias; }
        f32  GetNormalOffset() const    { return NormalOffsetTexels; }
        f32  GetPenumbra() const        { return PcfRadiusTexels; }
        f32  GetBlendBand() const       { return BlendBand; }
        bool GetDebugCascades() const   { return DebugCascades; }

    private:
        void CreateResources(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device);
        void CreateConstantBuffers(ID3D12Device* Device);
        void TransitionArray(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);

        D3D12_GPU_VIRTUAL_ADDRESS CascadeCBAddr(u32 Cascade) const {
            return CascadeCB->GetGPUVirtualAddress() +
                   (static_cast<UINT64>(FrameSlot) * kNumCascades + Cascade) *
                       sizeof(ShadowCascadeConstants);
        }

        Microsoft::WRL::ComPtr<ID3D12Resource>      DepthArray;
        FDescriptorHeap                             DSVHeap; 
        u32                                         ShadowSRVSlot_ = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES                       ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OpaquePSO; 
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskedPSO; 

        Microsoft::WRL::ComPtr<ID3D12Resource>      CascadeCB; 
        u8*                                         MappedCascade = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource>      CSMCB;      
        u8*                                         MappedCSM = nullptr;
        CSMConstants                                CPUConstants{};
        Mat44                                       CascadeViewProj[kNumCascades]{};

        u32  FrameSlot = 0;
        f32  ShadowMaxDistance   = 800.0f; 
        f32  DistributionExponent = 3.0f;  
        f32  DepthBias           = 0.0006f; 
        f32  NormalOffsetTexels  = 2.5f;    
        f32  CasterPullback      = 80.0f;   
        f32  PcfRadiusTexels     = 2.5f;    
        f32  BlendBand           = 0.1f;    
        bool DebugCascades       = false;  
        bool Initialized         = false;
    };
}
