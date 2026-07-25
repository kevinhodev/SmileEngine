#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
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
        Vec4  SunColor;        // rgb = cor do sol, w = ShadowRayMask (mask dos shadow rays no hit)
        Vec4  TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = shadowRayBias
                               // (so sombras no hit; origem de raio do G-buffer usa offset robusto)
        Vec4  ShadeParams;     // x = realHitShading (0/1), y = albedoLOD, z = fireflyMax, w = validateInterval
        Vec4  ReuseParams;     // x = MCap, y = posRejectScale, z = visibility (0/1), w = temporal (0/1)
        Vec4  SpatialParams;   // x = radius(px), y = count, z = spatial (0/1), w = normalReject
        Vec4  JitterParams;    // xy = prevJitterUv - currJitterUv (reprojecao temporal no espaco jittered)
        Mat44 View;            // anexado p/ o pack do NRD (worldPos -> view.z = IN_VIEWZ)
        // Perfil de epsilons (FRayEpsilonProfile). Anexado no FIM p/ nao deslocar nenhum offset
        // existente — em especial o View, que o ReSTIRNrdPack le em 256.
        Vec4  RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
        Vec4  RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
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
                            u32 DepthSlot, u32 GBufferSlot,
                            u32 VelocitySlot);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Vec3& CameraPos,
                            u32 Width, u32 Height, const Vec3& SunDir, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, f32 SkyIntensity,
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

        // Perfil compartilhado de epsilons. O Renderer empurra todo frame (copia barata) e e ele
        // quem invalida na borda de mudanca — aqui invalidar seria NeedsClear todo frame.
        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }

        bool IsReady() const   { return Ready; }
        // true quando a tabela t16 aponta p/ a OUT do NRD (radiancia em YCoCg) e nao p/ a
        // GITexture crua (RGB linear). Quem le o alvo precisa saber qual dos dois recebeu.
        bool IsNrdOutput() const  { return UseNrd && NrdOutSRV != kInvalidSlot; }
        // Tabela t16 do deferred: NRD OUT quando o NRD esta ligado, senao a GITexture crua.
        u32  GITexSRVSlot() const { return IsNrdOutput() ? NrdOutSRV : GITexSRV; }
        // As duas pontas, para quem precisa de uma especifica em vez da vigente (visualizador
        // de debug: o sinal CRU e o que mostra ruido/convergencia; o do NRD, o resultado).
        u32  GITexRawSRVSlot() const { return GITexSRV; }
        u32  NrdOutSRVSlot() const   { return NrdOutSRV; }

        // Invalida o historico temporal: arma o clear dos reservoirs no proximo RecordTrace.
        // Chamar em toggles e mudancas discretas de cena/iluminacao (o continuo — sol do
        // TimeOfDay — e coberto pela validacao periodica no shader, ver ValidateInterval).
        void InvalidateHistory()   { NeedsClear = true; }

        // Invalidam o historico: mudam o Lo JA GRAVADO nos reservoirs, e com ValidateInterval = 0
        // (config estavel atual) nao ha re-shade periodico — sem o clear, a radiancia do modo
        // anterior sobrevive ate a reprojecao rejeitar por posicao, e o toggle fica meio aplicado.
        void SetRealHitShading(bool V) { if (V != RealHit) NeedsClear = true; RealHit = V; }
        bool GetRealHitShading() const { return RealHit; }
        void SetTemporal(bool V)   { if (V && !Temporal) NeedsClear = true; Temporal = V; }
        bool GetTemporal() const   { return Temporal; }
        void SetFoliageShadows(bool V) { if (V != FoliageShadows) NeedsClear = true;
                                         FoliageShadows = V; }
        bool GetFoliageShadows() const { return FoliageShadows; }
        // NAO invalidam: o espacial nao realimenta o temporal, entao nem Spatial nem Visibility
        // (que so atua no Pass B e no resolve final) tocam o que esta gravado no reservoir.
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
        // Indexadas pela PARIDADE do ping-pong (FrameParity), nao pelo FrameSlot do frame em voo:
        // o conteudo da tabela depende de qual conjunto de reservoirs e prev e qual e curr.
        // TraceTable[p]    = 14 SRVs [TLAS,sky,inst,irrad,inst,inst,depth,gbuf,vel,prevA..D,luzes]
        //                    (prev = Res*[1-p]; o 14o, t13, e reescrito por frame — ver
        //                     SetPunctualLightsSRV, que DEVE usar a mesma paridade do RecordTrace).
        // TraceUAVTable[p] = 5 UAVs  [GITex, currA..D] (curr = Res*[p]).
        // SpatialTable[p]  = 10 SRVs [TLAS, currA..D, gbuf, depth, inst, inst, inst].
        static constexpr u32 kParityTables = 2;
        u32 TraceTable[kParityTables]    = { kInvalidSlot, kInvalidSlot };
        u32 TraceUAVTable[kParityTables] = { kInvalidSlot, kInvalidSlot };
        u32 SpatialTable[kParityTables]  = { kInvalidSlot, kInvalidSlot };
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
        bool NeedsClear  = false;
        bool Initialized = false;
        bool Ready       = false;

        // Tunaveis.
        bool RealHit        = true;
        f32  AlbedoLOD      = 2.0f;
        bool Temporal       = true;
        bool Spatial        = true;   // reuso espacial (off = só temporal = A2)
        bool UseNrd         = false;  // denoise via NRD RELAX (Fase C); off = ReSTIR cru no deferred
        bool FoliageShadows = true;   // folhagem nos shadow rays do hit (mask ALL vs OPAQUE)
        bool Visibility     = false;  // visibility rays no espacial: shading visibility (1 raio) +
                                      // visibilidade nos pesos MIS da correcao de bias (ate K raios).
                                      // Off por padrao (custo); toggle no editor p/ A/B
        f32  MCap           = 20.0f;
        // MaxAge saiu daqui p/ o FRayEpsilonProfile: virou knob de calibracao junto com os
        // epsilons de raio, e o perfil e compartilhado com reflexoes/DDGI.
        f32  PosRejectScale = 0.01f;
        f32  ValidateInterval = 0.0f; // re-shade periodico da amostra temporal: 0 = off (config
                                      // estavel do bisect 2026-07-12). ATENCAO: com TimeOfDay
                                      // animando, radiancia velha persiste no reservoir — religar
                                      // com 8 quando o sol dinamico voltar a importar
        f32  FireflyMax     = 8.0f;   // teto de luminancia do sample (anti-firefly; 0 = off) — caminho NRD
        f32  FireflyMaxRaw  = 4.0f;   // teto mais apertado p/ GI CRU (RR/None): sem o NRD limpando o
                                      // residuo, o RR/deferred mostra os sparkles direto (0 = off)
        f32  SpatialRadius  = 16.0f;  // raio (px) dos vizinhos
        f32  SpatialCount   = 4.0f;   // nº de vizinhos
        f32  NormalReject   = 0.9f;   // dot(n_q, n_r) minimo

        // Perfil compartilhado (dono = Renderer). Escrito em RayEpsA/B + TraceParams.w.
        FRayEpsilonProfile RayEps;
    };
}
