#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FCloudNoise;

    struct alignas(256) CloudConstants {
        Mat44 InvViewProjNoTrans;
        Vec4  CameraPos;    // xyz = camera (atmosphere km-frame), w = view height
        Vec4  SunDir;       // xyz = direction TO sun, w = sun intensity
        Vec4  SunColor;     // rgb, w unused
        Vec4  PlanetRadii;  // x = bottomR, y = cloudInnerR, z = cloudOuterR (km)
        Vec4  CloudParams;  // x = coverage, y = densityScale, z = noiseScale, w = time
        Vec4  CloudParams2; // x = weatherScale, y = erosionStrength, z = detailScale, w = cloudTypeBias
        Vec4  WindParams;   // xyz = wind velocity (km/s), w = frame index (jitter IGN)
        Vec4  MarchParams;  // x = primary steps, y = light steps, z unused, w = max march dist (km)
        Vec4  ScreenParams; // x = rtW, y = rtH, z = 1/rtW, w = 1/rtH
        Vec4  PhaseParams;  // x = g1, y = g2, z = blend, w = powderStrength
        Vec4  AtmoLink;     // x = atmoTopR, y = msOctaves, z = ambientScale, w unused
        Mat44 PrevVPWorld;     // VP do frame anterior, unidades de mundo, SEM jitter (parallax temporal)
        Vec4  TemporalParams;  // x = blend alpha, y = historia valida (0/1)
        Vec4  SkyAmbientCol;   // xyz = ambient fisico do ceu (F3), w unused
        Vec4  GroundAmbientCol;// xyz = ambient fisico do chao, w unused
        Mat44 InvViewProjWorld;// inversa full (jittered) p/ reconstruir world pos do depth
        Vec4  CamWorld;        // xyz = camera em unidades de mundo, w = km por unidade de mundo
        Vec4  CloudMisc;       // x = largura full-res, y = altura full-res, zw unused
    };

    class FVolumetricClouds {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        FCloudNoise& Noise, u32 AtmoTransmittanceSRV, u32 AtmoMultiScatterSRV,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat,
                        u32 Width, u32 Height);

        void RecreateComposite(ID3D12Device* Device,
                               DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Resize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                    u32 Width, u32 Height);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProjNoTranslation,
                            const Mat44& InvViewProjWorld, const Mat44& ViewProjWorldUnjittered,
                            const Vec3& CamWorldPos, f32 KmPerWorldUnit, f32 GroundRadiusKm,
                            const Vec3& DirToSun, const Vec3& SunColor,
                            const Vec3& SkyAmbient, const Vec3& GroundAmbient,
                            f32 Time, u32 FrameIndex);

        // DepthSRVSlot: depth da cena como SRV (raymarch clampa a marcha na geometria;
        // composite faz upsample bilateral). Chamador transiciona o depth p/ leitura antes.
        void RecordRaymarch(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                            u32 DepthSRVSlot);
        void Composite(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                       u32 DepthSRVSlot);

        bool IsInitialized() const { return Initialized; }

        // Half-res: pede recriacao do RT — o Renderer deve dar Flush na fila e chamar Resize.
        void SetHalfRes(bool V)         { HalfRes = V; }
        bool GetHalfRes() const         { return HalfRes; }
        void SetUseTemporal(bool V)     { if (V && !UseTemporal) HistoryValid = false; UseTemporal = V; }
        bool GetUseTemporal() const     { return UseTemporal; }
        void InvalidateHistory()        { HistoryValid = false; }
        void SetMarchSteps(f32 V)       { CPUConstants.MarchParams.X = V; }
        f32  GetMarchSteps() const      { return CPUConstants.MarchParams.X; }
        void SetAmbientScale(f32 V)     { CPUConstants.AtmoLink.Z = V; }
        f32  GetAmbientScale() const    { return CPUConstants.AtmoLink.Z; }
        void SetCloudTypeBias(f32 V)    { CPUConstants.CloudParams2.W = V; }
        f32  GetCloudTypeBias() const   { return CPUConstants.CloudParams2.W; }
        void SetPeakVariation(f32 V)    { CPUConstants.AtmoLink.W = V; }
        f32  GetPeakVariation() const   { return CPUConstants.AtmoLink.W; }

        // Troca a fonte do weather map (procedural re-bakeado ou textura autorada):
        // recopia a entrada t2 da tabela de descriptors e invalida a historia temporal.
        void SetWeatherSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Slot);
        void SetCoverage(f32 V)         { CPUConstants.CloudParams.X  = V; }
        void SetDensityScale(f32 V)     { CPUConstants.CloudParams.Y  = V; }
        void SetWindSpeed(f32 V)        { CPUConstants.WindParams.X = V; CPUConstants.WindParams.Z = V * 0.4f; }
        void SetErosion(f32 V)          { CPUConstants.CloudParams2.Y = V; }
        void SetPhaseG(f32 V)           { CPUConstants.PhaseParams.X  = V; }
        void SetPowder(f32 V)           { CPUConstants.PhaseParams.W  = V; }
        void SetAltitude(f32 BottomKm, f32 ThicknessKm) {
            CPUConstants.PlanetRadii.Y = CPUConstants.PlanetRadii.X + BottomKm;
            CPUConstants.PlanetRadii.Z = CPUConstants.PlanetRadii.X + BottomKm + ThicknessKm;
        }
        f32  GetCoverage() const       { return CPUConstants.CloudParams.X; }
        f32  GetDensityScale() const   { return CPUConstants.CloudParams.Y; }
        f32  GetWindSpeed() const      { return CPUConstants.WindParams.X; }
        f32  GetErosion() const        { return CPUConstants.CloudParams2.Y; }
        f32  GetPhaseG() const         { return CPUConstants.PhaseParams.X; }
        f32  GetPowder() const         { return CPUConstants.PhaseParams.W; }
        f32  GetBottomAltitude() const { return CPUConstants.PlanetRadii.Y - CPUConstants.PlanetRadii.X; }
        f32  GetThickness() const      { return CPUConstants.PlanetRadii.Z - CPUConstants.PlanetRadii.Y; }

    private:
        void CreateRT(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height);
        void BuildRaymarchPipeline(ID3D12Device* Device);
        void TransitionRT(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES After);
        void TransitionHist(ID3D12GraphicsCommandList* CommandList, u32 Index,
                            D3D12_RESOURCE_STATES After);
        void CreateConstantBuffer(ID3D12Device* Device);
        void BuildNoiseTable(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, FCloudNoise& Noise,
                             u32 AtmoTransmittanceSRV, u32 AtmoMultiScatterSRV);
        void BuildCompositeRootSignature(ID3D12Device* Device);
        void BuildCompositePSO(ID3D12Device* Device,
                               DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        // Raymarch tem root sig propria (tabela extra p/ o depth da cena, handle direto —
        // sem copia de descriptor que envelhece no resize). Temporal usa o pipeline comum.
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RaymarchRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> RaymarchPSOState;
        FVolumetricPipeline TemporalPSO;
        u32 NoiseTableStart = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> CloudRT;
        u32 RTSRVSlot = kInvalidSlot;
        u32 RTUAVSlot = kInvalidSlot;
        D3D12_RESOURCE_STATES RTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        u32 RTWidth = 0, RTHeight = 0;      // dimensoes do RT (half-res quando HalfRes)
        u32 FullWidth = 0, FullHeight = 0;  // resolucao de render (destino do composite)
        bool HalfRes = true;                // raymarch em meia resolucao + upsample bilinear

        // Acumulacao temporal: historico ping-pong na resolucao do RT.
        Microsoft::WRL::ComPtr<ID3D12Resource> HistoryRT[2];
        u32 HistSRVSlot[2] = { kInvalidSlot, kInvalidSlot };
        u32 HistUAVSlot[2] = { kInvalidSlot, kInvalidSlot };
        D3D12_RESOURCE_STATES HistState[2] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        u32  TemporalSRVTable[2] = { kInvalidSlot, kInvalidSlot }; // [raw, hist[i]] contiguos
        u32  HistIndex        = 0;     // indice do historico que sera ESCRITO neste frame
        u32  CompositeSRVSlot = kInvalidSlot; // fonte do composite (raw ou historico atual)
        bool UseTemporal   = true;
        bool HistoryValid  = false;
        bool HasPrevVP     = false;
        Mat44 StoredVPWorld = Mat44::Identity();

        Microsoft::WRL::ComPtr<ID3D12RootSignature> CompositeRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositePSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8*             MappedBase = nullptr;
        u32             FrameSlot  = 0;
        CloudConstants  CPUConstants{};

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(CloudConstants);
        }
        CloudConstants* Mapped() const {
            return reinterpret_cast<CloudConstants*>(
                MappedBase + static_cast<size_t>(FrameSlot) * sizeof(CloudConstants));
        }

        bool Initialized = false;
    };
}
