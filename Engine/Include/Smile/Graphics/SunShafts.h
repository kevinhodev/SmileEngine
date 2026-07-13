#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {

    // Sun shafts screen-space (radial blur), hibrido Cry/UE: mascara meia-res
    // (luminancia acima do limiar x distancia) -> 3 passes de blur radial de 12 taps
    // com distancia exponencial -> composicao aditiva no HDR antes do TAA/FSR2.
    // Funciona pra qualquer key light direcional (sol de dia, lua a noite).
    class FSunShafts {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        DXGI_FORMAT HDRFormat, u32 Width, u32 Height);
        // Recria os RTs meia-res quando a resolucao de render muda.
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        // SunUV = posicao projetada da key light na tela (pode sair de [0,1]);
        // Fade = 0 quando a key light esta atras da camera. Tint = cor da key light
        // normalizada (a energia real vem do proprio HDR mascarado).
        void UpdatePerFrame(u32 FrameSlot, const Vec2& SunUV, f32 Fade,
                            const Mat44& InvViewProjFull, const Vec3& CameraWorldPos,
                            const Vec3& Tint);

        // Mascara + blurs nos RTs proprios. HDR e depth devem estar em
        // PIXEL_SHADER_RESOURCE; nao mexe no render target do chamador.
        void RecordMaskAndBlur(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                               u32 HDRSRVSlot, u32 DepthSRVSlot);

        // F2 — inscatter direcional volumetrico (raymarch meia-res amostrando o CSM).
        // O RT resultante e consumido pelo fog apply (t2) no lugar do termo analitico.
        void UpdateVolumetric(const Vec3& DirToSun, const Vec3& SunColorTimesIntensity,
                              const Vec4& CollapsedFogParams, f32 NoiseFrame);
        // Depth em PIXEL_SHADER_RESOURCE e CSM legivel (EnsureReadable) antes. Muda
        // viewport pra meia-res — o chamador re-seta o viewport full depois.
        void RecordVolumetric(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                              u32 DepthSRVSlot, D3D12_GPU_VIRTUAL_ADDRESS CSMConstantsAddr,
                              u32 CSMShadowSRVSlot);
        u32  VolumetricSRVSlot() const { return VolSRVSlot; }
        // Composicao aditiva no HDR (chamador ja re-bindou o RTV e o viewport full-res).
        void Composite(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                       u32 FullWidth, u32 FullHeight);

        bool IsInitialized() const { return Initialized; }
        // false quando a key light esta atras da camera — pula os passes do frame
        bool HasWork() const       { return LastFade > 0.001f; }

        void SetIntensity(f32 V)      { Intensity = V; }
        f32  GetIntensity() const     { return Intensity; }
        void SetThreshold(f32 V)      { Threshold = V; }
        f32  GetThreshold() const     { return Threshold; }
        void SetRayLength(f32 V)      { FirstPassDistance = V; }
        f32  GetRayLength() const     { return FirstPassDistance; }
        void SetOcclusionRange(f32 V) { OcclusionDepthRange = V; }
        f32  GetOcclusionRange() const { return OcclusionDepthRange; }

        // F2 — volumetrico
        void SetVolumetric(bool V)      { VolEnabled = V; }
        bool GetVolumetric() const      { return VolEnabled; }
        void SetVolIntensity(f32 V)     { VolIntensity = V; }
        f32  GetVolIntensity() const    { return VolIntensity; }
        void SetVolPhaseG(f32 V)        { VolPhaseG = V; }
        f32  GetVolPhaseG() const       { return VolPhaseG; }
        void SetVolSteps(f32 V)         { VolSteps = V; }
        f32  GetVolSteps() const        { return VolSteps; }
        void SetVolMaxDist(f32 V)       { VolMaxDist = V; }
        f32  GetVolMaxDist() const      { return VolMaxDist; }

    private:
        struct alignas(256) VolConstants {
            Vec4  SunDirPhase;    // xyz = dir p/ a key light, w = g da fase HG
            Vec4  SunColorInt;    // rgb = cor da key light x intensidade da luz, w = intensidade do efeito
            Vec4  FogDensityP;    // x = dens1 colapsada, y = falloff1, z = dens2 colapsada, w = falloff2
            Vec4  MarchParams;    // x = passos, y = dist max, z = frame IGN, w unused
            Vec4  ScreenParams;   // dims do RT meia-res
            Mat44 InvViewProj;
            Vec4  CameraWorldPos;
        };

        struct alignas(256) ShaftConstants {
            Vec4  SunPosParams;   // xy = UV do sol, z = fade, w = aspect (H/W)
            Vec4  ShaftParams;    // x = limiar, y = dist 1o passe, z = escala do passe, w = intensidade
            Vec4  ShaftTint;      // rgb = tint, w = 1/occlusionDepthRange
            Vec4  ScreenParams;   // xy = dims do RT alvo, zw = 1/dims
            Mat44 InvViewProj;
            Vec4  CameraWorldPos;
        };

        void BuildRootSignature(ID3D12Device* Device);
        void BuildVolRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT HDRFormat);
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateRTs(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        static constexpr u32 kBlurPasses  = 3;
        // regioes de CB por frame: mask, blur 0..2, apply
        static constexpr u32 kCBRegions   = 2 + kBlurPasses;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> MaskPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> BlurPSO;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> ApplyPSO;

        // F2 — volumetrico: root sig propria (CSM em b3/t11/s2, registers do CSMCommon)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> VolRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> VolPSO;
        Microsoft::WRL::ComPtr<ID3D12Resource> VolRT;
        u32 VolSRVSlot = kInvalidSlot;
        D3D12_RESOURCE_STATES VolState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D12Resource> VolCB;
        u8* VolMappedBase = nullptr;
        bool VolEnabled   = true;
        f32  VolIntensity = 1.0f;
        f32  VolPhaseG    = 0.7f;
        f32  VolSteps     = 16.0f;
        f32  VolMaxDist   = 400.0f;

        // ping-pong meia-res RGBA16F (mask -> blur x3)
        Microsoft::WRL::ComPtr<ID3D12Resource> ShaftRT[2];
        FDescriptorHeap RTVHeap;
        u32 RTSRVSlot[2] = { kInvalidSlot, kInvalidSlot };
        D3D12_RESOURCE_STATES RTState[2] = { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        u32 RTWidth = 0, RTHeight = 0;
        u32 FinalRT = 1; // indice do RT com o resultado do ultimo blur

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8* MappedBase = nullptr;
        u32 FrameSlot  = 0;

        // copias CPU do ultimo UpdatePerFrame (upload heap e write-combined; nao ler dele)
        Mat44 LastInvViewProj = Mat44::Identity();
        Vec3  LastCameraPos{};

        f32 LastFade            = 0.0f;
        f32 Intensity           = 0.3f; // 0.7 estourava no A/B (bola branca)
        f32 Threshold           = 2.0f;
        f32 FirstPassDistance   = 0.1f;    // fracao da distancia ate o sol no 1o passe
        f32 OcclusionDepthRange = 1000.0f; // so geometria alem de range/2 alimenta a mascara

        void Transition(ID3D12GraphicsCommandList* CommandList, u32 Index,
                        D3D12_RESOURCE_STATES After);

        ShaftConstants* Region(u32 Pass) const {
            return reinterpret_cast<ShaftConstants*>(
                MappedBase + (static_cast<size_t>(FrameSlot) * kCBRegions + Pass) *
                                 sizeof(ShaftConstants));
        }
        D3D12_GPU_VIRTUAL_ADDRESS RegionAddr(u32 Pass) const {
            return ConstantBuffer->GetGPUVirtualAddress() +
                   (static_cast<UINT64>(FrameSlot) * kCBRegions + Pass) * sizeof(ShaftConstants);
        }

        bool Initialized = false;
    };
}
