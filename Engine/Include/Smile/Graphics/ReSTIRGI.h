#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // Constantes do ReSTIR GI (b0 dos passes). alignas(256); casa campo-a-campo com o cbuffer de
    // ReSTIRGITrace.cs.hlsl / ReSTIRGISpatial.cs.hlsl (CB compartilhado pelos 2 passes).
    struct alignas(256) ReSTIRGIConstants {
        Mat44 InvViewProj;     // inversa FULL da view-proj (row-major)
        Vec4  CameraPos;       // xyz = camera mundo
        Vec4  ScreenParams;    // W, H, 1/W, 1/H
        Vec4  GridMinSpacing;  // DDGI: xyz = origem do grid, w = espacamento
        Vec4  GridCount;       // DDGI: xyz = nº de probes por eixo
        Vec4  AtlasParams;     // DDGI irradiancia: x = tile, y = W, z = H
        Vec4  SunDirIntensity; // xyz = direcao P/ o sol (norm.), w = intensidade
        Vec4  SunColor;        // rgb = cor do sol
        Vec4  TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = shadowRayBias
                               // (so sombras no hit; origem de raio do G-buffer usa offset robusto)
        Vec4  ShadeParams;     // x = realHitShading (0/1), y = albedoLOD, z = fireflyMax, w = validateInterval
        Vec4  ReuseParams;     // x = MCap, y = posRejectScale, z = visibility (0/1), w = temporal (0/1)
        Vec4  SpatialParams;   // x = radius(px), y = count, z = spatial (0/1), w = normalReject
        Vec4  JitterParams;    // xy = prevJitterUv - currJitterUv (reprojecao temporal no espaco jittered)
        Mat44 View;            // anexado p/ o pack do NRD (worldPos -> view.z = IN_VIEWZ)
        Vec4  NrdHitDistParams;// xyz = ReblurHitDistanceParameters {A,B,C} (igual ao driver NRD)
    };

    // ReSTIR GI — final-gather difuso por pixel sobre o DDGI (radiance cache). Molde do FReflections.
    // A3: Pass A (trace + reservoir temporal) -> Pass B (reuso espacial + Jacobiano + resolve).
    // Reservoir {x1,x2,n2,Lo,M,W} em 4 tex ping-pong. Atras do toggle UseReSTIRGI (default OFF).
    class FReSTIRGI {
    public:
        void Initialize(ID3D12Device* Device);

        void SetGIParams(const Vec3& GridMin, f32 Spacing, const Vec3& GridCount,
                         f32 AtlasTile, f32 AtlasW, f32 AtlasH, f32 MaxRayDist);

        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height,
                            u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot, u32 IrradSlot,
                            u32 VertexSlot, u32 IndexSlot, u32 DepthSlot, u32 GBufferSlot,
                            u32 VelocitySlot);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Vec3& CameraPos,
                            u32 Width, u32 Height, const Vec3& SunDir, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, f32 SkyIntensity, f32 ShadowRayBias,
                            const Mat44& View, const Vec2& JitterDeltaUv,
                            u32 PunctualLightCount = 0);

        // F5: copia o SRV do buffer de luzes puntuais do frame pro t13 da tabela de trace da
        // paridade CORRENTE (a outra pertence ao frame em voo — descriptor versioning).
        void SetPunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  u32 StagingSlot);

        void RecordTrace(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // NRD (Fase C): cria o pack pipeline + UAVs das IN textures do NRD + SRV da OUT (no SRVHeap
        // da engine). Chamar apos SetupForResize (depende dos slots cacheados) e do Nrd.SetupForResize.
        void SetupNrdPack(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                          ID3D12Resource* NrdInViewZ, ID3D12Resource* NrdInNormalRough,
                          ID3D12Resource* NrdInMv, ID3D12Resource* NrdInDiffRadHit,
                          ID3D12Resource* NrdOut);
        // Empacota os inputs do NRD a partir da GITexture + gbuffer/depth/velocity. Caller transicionou
        // as IN do NRD p/ UAV e o depth/gbuffer/velocity p/ leitura.
        void RecordNrdPack(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        void SetUseNrd(bool V) { UseNrd = V; }
        bool GetUseNrd() const { return UseNrd; }

        bool IsReady() const   { return Ready; }
        // Tabela t16 do deferred: NRD OUT quando o NRD esta ligado, senao a GITexture crua.
        u32  GITexSRVSlot() const { return (UseNrd && NrdOutSRV != kInvalidSlot) ? NrdOutSRV : GITexSRV; }

        // Invalida o historico temporal: arma o clear dos reservoirs no proximo RecordTrace.
        // Chamar em toggles e mudancas discretas de cena/iluminacao (o continuo — sol do
        // TimeOfDay — e coberto pela validacao periodica no shader, ver ValidateInterval).
        void InvalidateHistory()   { NeedsClear = true; }

        void SetRealHitShading(bool V) { RealHit = V; }
        bool GetRealHitShading() const { return RealHit; }
        void SetTemporal(bool V)   { if (V && !Temporal) NeedsClear = true; Temporal = V; }
        bool GetTemporal() const   { return Temporal; }
        void SetSpatial(bool V)    { Spatial = V; }
        bool GetSpatial() const    { return Spatial; }
        void SetVisibility(bool V) { Visibility = V; }
        bool GetVisibility() const { return Visibility; }

    private:
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void CreateConstantBuffer(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FVolumetricPipeline TracePSO;   // 14 SRV, 5 UAV, heap-directly-indexed (Pass A)
        FVolumetricPipeline SpatialPSO; // 10 SRV, 1 UAV, heap-directly-indexed (Pass B; alpha-test M6)
        FVolumetricPipeline NrdPackPSO; // 4 SRV [GITex,gbuf,depth,vel], 4 UAV [NRD IN] (Fase C)

        Microsoft::WRL::ComPtr<ID3D12Resource> GITexture;
        D3D12_RESOURCE_STATES GITextureState = D3D12_RESOURCE_STATE_COMMON;
        // Reservoir ping-pong: A=RGBA32F[x1,M], B=RGBA32F[x2,W], C=RGBA16F[Lo], D=RGBA16F[n2].
        Microsoft::WRL::ComPtr<ID3D12Resource> ResA[2], ResB[2], ResC[2], ResD[2];
        D3D12_RESOURCE_STATES ResAState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES ResBState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES ResCState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES ResDState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 GITexSRV   = kInvalidSlot;
        u32 GITexUAV   = kInvalidSlot;
        u32 ResASRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResBSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResCSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResDSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResAUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResBUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResCUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResDUAV[2] = { kInvalidSlot, kInvalidSlot };
        // Por paridade: TraceTable[p] = 13 SRVs [TLAS,sky,inst,irrad,verts,idx,depth,gbuf,vel,prevA..D]
        // (prev = Res*[1-p]); TraceUAVTable[p] = 5 UAVs [GITex,currA..D] (curr = Res*[p]).
        // SpatialTable[p] = 7 SRVs [TLAS, currA..D, gbuf, depth].
        u32 TraceTable[2]    = { kInvalidSlot, kInvalidSlot };
        u32 TraceUAVTable[2] = { kInvalidSlot, kInvalidSlot };
        u32 SpatialTable[2]  = { kInvalidSlot, kInvalidSlot };
        // NRD pack (Fase C). PackSrvTable = [GITex,gbuf,depth,vel]; PackUavTable = [viewZ,nr,mv,radHit].
        u32 PackSrvTable = kInvalidSlot;
        u32 PackUavTable = kInvalidSlot;
        u32 NrdOutSRV    = kInvalidSlot; // SRV da OUT do NRD (tabela t16 quando UseNrd)
        // Slots cacheados do SetupForResize (p/ montar as tabelas do pack).
        u32 DepthSlot = kInvalidSlot, GBufferSlot = kInvalidSlot, VelocitySlot = kInvalidSlot;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB  = nullptr;
        u32 FrameSlot = 0;
        ReSTIRGIConstants CPU{};

        Vec4 GIGridMinSpacing{ 0, 0, 0, 1 };
        Vec4 GIGridCount{ 0, 0, 0, 0 };
        Vec4 GIAtlasParams{ 6, 1, 1, 0 };
        f32  GIMaxRayDist = 0.0f;

        u32  Width = 0, Height = 0;
        u32  FrameParity = 0;
        u32  CurrParity  = 0;
        bool NeedsClear  = false;
        bool Initialized = false;
        bool Ready       = false;

        // Tunaveis.
        bool RealHit        = true;
        f32  AlbedoLOD      = 2.0f;
        bool Temporal       = true;
        bool Spatial        = true;   // reuso espacial (off = só temporal = A2)
        bool UseNrd         = false;  // denoise via NRD RELAX (Fase C); off = ReSTIR cru no deferred
        bool Visibility     = false;  // visibility rays no espacial: shading visibility (1 raio) +
                                      // visibilidade nos pesos MIS da correcao de bias (ate K raios).
                                      // Off por padrao (custo); toggle no editor p/ A/B
        f32  MCap           = 20.0f;
        f32  PosRejectScale = 0.01f;
        f32  ValidateInterval = 8.0f; // re-shade da amostra temporal em 1/N dos px por frame
                                      // (radiancia envelhece com sol dinamico); 0 = off
        f32  FireflyMax     = 8.0f;   // teto de luminancia do sample (anti-firefly; 0 = off)
        f32  SpatialRadius  = 16.0f;  // raio (px) dos vizinhos
        f32  SpatialCount   = 4.0f;   // nº de vizinhos
        f32  NormalReject   = 0.9f;   // dot(n_q, n_r) minimo
    };
}
