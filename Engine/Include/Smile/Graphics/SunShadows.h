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

    // Constantes de amostragem das cascatas (CB b3, lido pelo Triangle.ps via CSMCommon.hlsli).
    // Bate campo-a-campo com o cbuffer CSMCB. WorldToShadow inclui o mapeamento NDC->UV.
    struct alignas(256) CSMConstants {
        Mat44 WorldToShadow[4];  // 4*64 = 256 bytes
        Vec4  CascadeTexelWorld; // x..w = tamanho de 1 texel em mundo, por cascata (normal-offset)
        Vec4  Params;            // x = numCascades, y = depthBias (NDC z), z = 1/res, w = enabled
        Vec4  Params2;           // x = normal-offset (em texels), yzw reservado
    };

    // CB por-cascata do depth pass (b0 do ShadowDepth.vs): só a LightViewProj (clip ortho).
    struct alignas(256) ShadowCascadeConstants {
        Mat44 LightViewProj;
    };

    // Cascaded Shadow Maps para o sol direcional. Base ortográfica (estilo Unreal):
    // por cascata ajusta uma bounding-sphere ao sub-frustum da câmera, projeção ortográfica,
    // texel-snap (anti-shimmer, estilo Cry). Auto-contida (root sig + PSOs + CBs próprios),
    // espelhando o padrão de FAtmosphere/FWaterRenderer. Só renderização de depth; a
    // amostragem PCF vive no Triangle.ps (CSMCommon.hlsli).
    class FSunShadows {
    public:
        static constexpr u32 kNumCascades = 4;
        static constexpr u32 kResolution  = 2048;

        // Um caster do depth pass: mesh + material (alpha-test/two-sided) + endereço do
        // ObjectConstants já preenchido pelo Renderer (reusa o mesmo CB da cena) + AABB de
        // mundo (culling por cascata — casters fora do frustum da CÂMERA ainda projetam).
        struct FShadowDrawItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Mat;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCB;
            Vec3                      AABBMin;
            Vec3                      AABBMax;
        };

        // Cria a Texture2DArray de depth (kNumCascades fatias), DSVs por fatia, o SRV array
        // no SRVHeap da engine, a root sig + 2 PSOs (opaco/masked) e os CBs. SampleCount=1
        // sempre (shadow maps são single-sample; independentes da swapchain/MSAA).
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);

        // Por frame: calcula as cascatas (quando Enabled) e preenche o CSM CB + os CBs
        // por-cascata. View/CamPos/FovY/Aspect descrevem a câmera; DirToSun = direção P/ o sol.
        void UpdatePerFrame(u32 FrameSlot, bool Enabled, const Mat44& View, const Vec3& CamPos,
                            f32 FovYRadians, f32 Aspect, const Vec3& DirToSun, f32 NearZ);

        // Renderiza o depth das cascatas. Troca render targets/viewport/root sig (o caller
        // deve restaurar o estado da cena depois). Deixa a array em PIXEL_SHADER_RESOURCE.
        void RecordDepthPass(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                             const FShadowDrawItem* Items, size_t Count);

        // Garante a array legível por PS (quando o depth pass é pulado com sombras off).
        void EnsureReadable(ID3D12GraphicsCommandList* CommandList);

        u32  ShadowSRVSlot() const { return ShadowSRVSlot_; }
        D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress() const {
            return CSMCB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(CSMConstants);
        }
        bool IsInitialized() const { return Initialized; }

        // Tunáveis (calibração visual).
        void SetMaxDistance(f32 D)      { ShadowMaxDistance = D; }
        void SetDepthBias(f32 B)        { DepthBias = B; }
        void SetCasterPullback(f32 P)   { CasterPullback = P; }
        void SetNormalOffset(f32 Texels){ NormalOffsetTexels = Texels; } // anti peter-panning
        void SetPenumbra(f32 Texels)    { PcfRadiusTexels = Texels; }    // largura da soft shadow
        void SetBlendBand(f32 UV)       { BlendBand = UV; }              // transição entre cascatas
        void SetDebugCascades(bool On)  { DebugCascades = On; }          // tinge cada cascata (diagnóstico)
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
        FDescriptorHeap                             DSVHeap; // kNumCascades DSVs (1 por fatia)
        u32                                         ShadowSRVSlot_ = 0xFFFFFFFFu;
        D3D12_RESOURCE_STATES                       ArrayState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> OpaquePSO; // sem PS (cull back + slope bias)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskedPSO; // com PS alpha-test (cull none)

        Microsoft::WRL::ComPtr<ID3D12Resource>      CascadeCB; // ring: framesInFlight * kNumCascades
        u8*                                         MappedCascade = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource>      CSMCB;      // ring: framesInFlight
        u8*                                         MappedCSM = nullptr;
        CSMConstants                                CPUConstants{};
        // Cópia CPU da LightViewProj de cada cascata (sem BiasUV) p/ culling de casters
        // por cascata no depth pass.
        Mat44                                       CascadeViewProj[kNumCascades]{};

        u32  FrameSlot = 0;
        // Tunáveis.
        f32  ShadowMaxDistance   = 800.0f; // alcance do CSM (world units); CSM cobre só o entorno
        f32  DistributionExponent = 3.0f;  // >1 = cascatas menores perto da câmera
        // O normal-offset faz o grosso do anti-acne → bias NDC residual pequeno (anti peter-panning).
        f32  DepthBias           = 0.0006f; // bias NDC z residual no PCF
        f32  NormalOffsetTexels  = 2.5f;    // empurra o ponto amostrado ~N texels ao longo da normal
        f32  CasterPullback      = 80.0f;   // estende o near da ortho rumo ao sol (casters altos)
        // Fase 2 (soft shadows + blend).
        f32  PcfRadiusTexels     = 2.5f;    // raio do kernel Poisson (largura da penumbra)
        f32  BlendBand           = 0.1f;    // fração de UV na borda da cascata p/ blend (0 = off)
        bool DebugCascades       = false;   // tinge cada cascata (vermelho/verde/azul/amarelo)
        bool Initialized         = false;
    };
}
