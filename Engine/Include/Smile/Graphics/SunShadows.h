#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include <d3d12.h>
#include <functional>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;
    class FGpuProfiler;
    class FGpuMesh;
    class FMaterial;

    struct alignas(256) CSMConstants {
        Mat44 WorldToShadow[4];  // 4*64 = 256 bytes
        Vec4  CascadeTexelWorld; // x..w = tamanho de 1 texel em mundo, por cascata (normal-offset)
        Vec4  Params;            // x = numCascades, y = depthBias (NDC z), z = 1/res, w = enabled
        Vec4  Params2;           // x = normal-offset (em texels), yzw reservado
        Vec4  Params3;           // x = frame do ruido do PCF, y = tan(meio-angulo do sol; 0 = PCSS off), z = penumbra max (texels)
        Vec4  BiasScale;         // multiplicador do depth bias por cascata (default 1,1,1,1)
        Vec4  DepthRangeWorld;   // extensao em mundo do range de depth do ortho, por cascata (PCSS)
        Vec4  CascadeSplits;     // profundidade view-space do fim de cada cascata
        Vec4  CameraPosition;    // xyz = camera em mundo
        Vec4  CameraForwardNear; // xyz = frente da camera, w = near plane
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

        // Caster extra por cascata (terreno): chamado depois dos itens, com o CB e a matriz
        // da cascata. Pode trocar root signature/PSO — o loop re-liga os do CSM na proxima
        // cascata.
        using FExtraCascadeDraw = std::function<void(ID3D12GraphicsCommandList*, u32 Cascade,
                                                     D3D12_GPU_VIRTUAL_ADDRESS CascadeCB,
                                                     const Mat44& CascadeViewProj)>;

        void RecordDepthPass(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                             const FShadowDrawItem* Items, size_t Count,
                             const FExtraCascadeDraw& ExtraDraw = {},
                             FGpuProfiler* Profiler = nullptr);

        void EnsureReadable(ID3D12GraphicsCommandList* CommandList);
        // Leitura tambem em compute (volumetric fog): PIXEL | NON_PIXEL.
        void EnsureReadableCompute(ID3D12GraphicsCommandList* CommandList);

        u32  ShadowSRVSlot() const { return ShadowSRVSlot_; }
        D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress() const {
            return CSMCB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(CSMConstants);
        }
        bool IsInitialized() const { return Initialized; }

        void SetMaxDistance(f32 D)      { ShadowMaxDistance = D; InvalidateCache(); }
        void SetDepthBias(f32 B)        { DepthBias = B; }
        void SetCasterPullback(f32 P)   { CasterPullback = P; InvalidateCache(); }
        void SetNormalOffset(f32 Texels){ NormalOffsetTexels = Texels; }
        void SetPenumbra(f32 Texels)    { PcfRadiusTexels = Texels; }
        void SetBlendBand(f32 Fraction) { BlendBand = Fraction; InvalidateCache(); }
        void SetDebugCascades(bool On)  { DebugCascades = On; }
        f32  GetMaxDistance() const     { return ShadowMaxDistance; }
        f32  GetDepthBias() const       { return DepthBias; }
        f32  GetNormalOffset() const    { return NormalOffsetTexels; }
        f32  GetPenumbra() const        { return PcfRadiusTexels; }
        f32  GetBlendBand() const       { return BlendBand; }
        bool GetDebugCascades() const   { return DebugCascades; }

        // Cache de cascatas distantes (round-robin) + filtro de caster pequeno + bias por cascata.
        void SetCascadeCache(bool On)   { CacheEnabled = On; InvalidateCache(); }
        bool GetCascadeCache() const    { return CacheEnabled; }
        void SetMinCasterTexels(f32 T)  { MinCasterTexels = T; }
        f32  GetMinCasterTexels() const { return MinCasterTexels; }
        void SetCascadeBiasScale(u32 C, f32 S) { if (C < kNumCascades) CascadeBiasScale[C] = S; }
        f32  GetCascadeBiasScale(u32 C) const  { return C < kNumCascades ? CascadeBiasScale[C] : 1.0f; }

        // PCSS (contact hardening, cascata 0). Tamanho angular do sol em graus; 0 = off.
        void SetSunAngularSize(f32 Deg)   { SunAngularSizeDeg = Deg; }
        f32  GetSunAngularSize() const    { return SunAngularSizeDeg; }
        void SetMaxPenumbraTexels(f32 T)  { MaxPenumbraTexels = T; }
        f32  GetMaxPenumbraTexels() const { return MaxPenumbraTexels; }

    private:
        void InvalidateCache() { for (u32 c = 0; c < kNumCascades; ++c) CacheValid[c] = false; }

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
        // Fracao final de cada intervalo de view-depth usada no crossfade. A cascata
        // seguinte e ajustada com a mesma sobreposicao (contrato Flax/Unreal).
        f32  BlendBand           = 0.1f;
        bool DebugCascades       = false;
        bool Initialized         = false;

        // Cache round-robin: cascatas 2/3 re-renderizam a cada 2/4 frames (defasadas),
        // com a matriz congelada no CB entre updates. Invalidacao: sol girou alem do
        // limiar, esfera ideal escapou da congelada, ou parametros de fitting mudaram.
        bool CacheEnabled  = true;
        u32  UpdateMask    = 0xFu;              // cascatas re-renderizadas neste frame
        u64  UpdateCounter = 0;
        bool CacheValid[kNumCascades]  = {};
        Vec3 CachedFwd[kNumCascades]{};         // dir da luz no ultimo update
        Vec3 CachedCenter[kNumCascades]{};      // centro (snapped) da esfera congelada
        f32  CachedRadius[kNumCascades] = {};   // raio (com folga) da esfera congelada
        f32  MinCasterTexels = 2.0f;            // caster menor que N texels da cascata nao desenha (0 = off)
        f32  CascadeBiasScale[kNumCascades] = { 1.0f, 1.0f, 1.0f, 1.0f };
        f32  SunAngularSizeDeg = 0.53f;         // diametro angular do sol (PCSS); 0 = penumbra fixa
        f32  MaxPenumbraTexels = 8.0f;          // teto do kernel PCSS = raio do blocker search
    };
}
