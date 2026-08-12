#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/GIHitSampling.h"
#include "Smile/Graphics/DDGI.h" // FDDGICascadeConstants
#include "Smile/Graphics/ReGIR.h"
#include "Smile/Graphics/RadianceCache.h"
#include "Smile/Graphics/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>

namespace Smile {
    class FGpuProfiler;
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
        Vec4  ShadeParams;     // x = livre, y = albedoLOD, z = fireflyMax, w = validateInterval
        Vec4  ReuseParams;     // x = MCap, y = posRejectScale, z = visibility (0/1), w = temporal (0/1)
        Vec4  SpatialParams;   // x = radius(px), y = count, z = spatial (0/1), w = normalReject
        Vec4  JitterParams;    // xy = prevJitterUv - currJitterUv (reprojecao temporal no espaco jittered)
        Mat44 View;            // anexado p/ o pack do NRD (worldPos -> view.z = IN_VIEWZ)
        // Perfil de epsilons (FRayEpsilonProfile). Anexado no FIM p/ nao deslocar nenhum offset
        // existente — em especial o View, que o ReSTIRNrdPack le em 256.
        Vec4  RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
        Vec4  RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
        Vec4  PolicyParams;      // x = politica de backface no gather (0/1),
                                 // y = strength do boiling filter (0 = off),
                                 // z = correcao de vies do temporal (0/1),
                                 // w = kill de backface no Jacobiano (0 = abs() historico)
        // Gather do 2o bounce no hit (contrato do HitShading.hlsli): o mesmo sampler completo
        // do deferred, com Chebyshev e skip de sonda inativa.
        Vec4  GIDistParams;      // x=distTile, y=distAtlasW, z=distAtlasH, w=skipMode
        Vec4  GIBiasParams;      // x=escala do bias de superficie, y=teto em metros, zw=-
        Vec4  ReGIRGridMinSlots;
        Vec4  ReGIRInvCellEnabled;
        Vec4  ReGIRGridCountSamples;
        Vec4  ReGIRResources;
        // Parameterizacao do sky-view LUT p/ o ShadeSky do HitShading.hlsli, vinda do
        // FAtmosphere (fonte unica). Anexado no FIM p/ nao deslocar offset nenhum.
        Vec4  SkyParams;         // x = view height (km), y = raio do planeta (km), zw = livres
        // Instrumentacao de timer (FShaderTimer). Anexado no FIM pela mesma razao dos anteriores.
        Vec4  DebugParams;       // x = slot bindless do alvo de timer (< 0 = captura off)
        // Historico de superficie do FTemporalMotionVectors: de onde sai o x1 do reservoir
        // temporal desde que ele deixou de ser gravado (ver ReSTIRReservoir.hlsli).
        Vec4  HistoryParams;     // x = slot bindless do Surface do frame ANTERIOR
        // World radiance cache (FRadianceCacheShaderParams). Contrato por NOME com o
        // RC_UNPACK_PARAMS do RadianceCache.hlsli; anexado no FIM como os anteriores.
        Vec4  RadianceCacheCamCell;
        Vec4  RadianceCacheLodCapFlags;
        Vec4  RadianceCacheResources;
        // Cascatas do DDGI, para o gather do 2o bounce no HitShading. Mesmo bloco dos outros
        // quatro cbuffers; o GIGridMinSpacing continua sendo a GROSSA (peso do volume).
        FDDGICascadeConstants GICascades;
    };
    static_assert(offsetof(ReSTIRGIConstants, GICascades) == 560,
                  "bloco de cascatas anexado ao fim do cbuffer (ver Renderer.h)");
    static_assert(offsetof(ReSTIRGIConstants, ReGIRGridMinSlots) == 400,
                  "ReSTIRGIConstants divergiu do cbuffer ReSTIRCB");
    static_assert(offsetof(ReSTIRGIConstants, SkyParams) == 464,
                  "SkyParams deve permanecer anexado ao fim do ReSTIRCB");

    // ReSTIR GI — final-gather difuso por pixel sobre o DDGI (radiance cache). Molde do FReflections.
    // A3: Pass A (trace + reservoir temporal) -> Pass B (reuso espacial + Jacobiano + resolve).
    // Reservoir {x2,n2,Lo,M,W} em 2 tex ping-pong (x1 e reconstruido). Atras do toggle
    // UseReSTIRGI (default OFF).
    class FReSTIRGI : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "ReSTIR GI"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        void Initialize(ID3D12Device* Device);

        void SetGIParams(const Vec3& GridMin, f32 Spacing, const Vec3& GridCount,
                         f32 AtlasTile, f32 AtlasW, f32 AtlasH, f32 MaxRayDist);

        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, u32 Width, u32 Height,
                            u32 TlasSlot, u32 SkyViewSlot, u32 InstanceSlot, u32 IrradSlot,
                            u32 DepthSlot, u32 GBufferSlot, u32 VelocitySlot,
                            // t4/t5 do trace: atlas de distancia e ProbeData do DDGI — o 2o
                            // bounce usa o gather completo (Chebyshev + skip), nao a trilinear.
                            u32 DistSlot, u32 ProbeDataSlot);

        // PrevSurfaceSlot = SRV do historico de superficie do frame ANTERIOR
        // (FTemporalMotionVectors::SurfaceSRV(1 - FrameSlot)). E de la que o reuso temporal tira o
        // x1, que saiu do reservoir. kInvalidSlot DESLIGA o reuso temporal deste frame: sem
        // historico de superficie nao ha como reconstruir o ponto visivel anterior, e ler um
        // descriptor invalido seria pior que perder um frame de acumulo.
        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Vec3& CameraPos,
                            u32 Width, u32 Height, const Vec3& SunDir, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, f32 SkyIntensity,
                            const Mat44& View, const Vec2& JitterDeltaUv,
                            u32 PrevSurfaceSlot,
                            u32 PunctualLightCount = 0);

        // F5: copia o SRV do buffer de luzes puntuais do frame pro t13 da tabela de trace da
        // paridade CORRENTE (a outra pertence ao frame em voo — descriptor versioning).
        void SetPunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  u32 StagingSlot);

        void RecordTrace(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                         FGpuProfiler* Profiler = nullptr);

        // Instrumentacao de timer (ver FShaderTimer): kInvalidSlot desliga e o passe volta p/ a
        // PSO normal, sem uma instrucao a mais. Dono = Renderer, empurra todo frame.
        void SetTimerSlot(u32 Slot) { TimerSlot = Slot; }
        // Se a permutacao instrumentada existe (NVAPI + .cso presentes).
        bool HasTimerPipeline() const { return TraceTimed; }

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
        // Gather do 2o bounce (dono = Renderer, empurra todo frame; ver FGIHitSampling).
        void SetGIHitSampling(const FGIHitSampling& S) { GIHit = S; }
        // Cascatas: empurradas por FRAME (nao no SetGIParams, que so roda em setup/resize) porque
        // a origem da cascata fina segue a camera. Dono = Renderer, fonte = FDDGI.
        void SetGICascades(const FDDGICascadeConstants& C) { GICascadesCPU = C; }
        void SetReGIRParams(const FReGIRShaderParams& P) { ReGIRParams = P; }
        // COM o bit de update: o reservoir do ReSTIR GI ja guarda radiancia nao-direcional (e por
        // isso que o RoughnessMin existe aqui). Ver FRadianceCache::ShaderParams.
        void SetRadianceCacheParams(const FRadianceCacheShaderParams& P) { RadianceCacheParams = P; }
        // Parameterizacao do sky-view LUT p/ o ShadeSky dos raios que escapam (dono = Renderer,
        // empurra todo frame a partir do FAtmosphere — fonte unica, ver Atmosphere.h).
        void SetSkyParams(f32 ViewHeightKm, f32 BottomRadiusKm) {
            SkyLutParams = { ViewHeightKm, BottomRadiusKm, 0.0f, 0.0f };
        }

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
        void SetTemporal(bool V)   { if (V && !Temporal) NeedsClear = true; Temporal = V; }
        bool GetTemporal() const   { return Temporal; }
        void SetFoliageShadows(bool V) { if (V != FoliageShadows) NeedsClear = true;
                                         FoliageShadows = V; }

        // Politica de backface do gather e da revalidacao (retrace de auto-interseccao +
        // terminacao preta no verso one-sided). A UE liga a equivalente por default
        // (AvoidSelfIntersections=3), mas aqui o DEFAULT E OFF: com o gather tracando sem culling,
        // o backface ja bloqueia o raio por si, entao a terminacao preta so troca "quase preto"
        // por "exatamente zero" — diferenca invisivel na matriz 2x2 medida no alvo cru, contra um
        // HitIsBackface por raio. Fica como instrumento, a religar quando o custo/beneficio for
        // medido (ou se o culling voltar ao gather, quando ela deixa de ser inerte).
        void SetBackfacePolicy(bool V) { if (V != BackfacePolicy) NeedsClear = true;
                                         BackfacePolicy = V; }
        bool GetBackfacePolicy() const { return BackfacePolicy; }
        bool GetFoliageShadows() const { return FoliageShadows; }
        // NAO invalidam: o espacial nao realimenta o temporal, entao nem Spatial nem Visibility
        // (que so atua no Pass B e no resolve final) tocam o que esta gravado no reservoir.
        void SetSpatial(bool V)    { Spatial = V; }
        bool GetSpatial() const    { return Spatial; }
        void SetVisibility(bool V) { Visibility = V; }
        bool GetVisibility() const { return Visibility; }
        // Boiling filter: NAO invalida historico (so muda o corte de outlier deste frame em
        // diante), entao serve de A/B direto — 0 desliga e devolve o comportamento anterior.
        void SetBoilingStrength(f32 V) { BoilingStrength = V; }
        f32  GetBoilingStrength() const { return BoilingStrength; }
        // Correcao de vies do reuso temporal (estilo RTXDI Basic). INVALIDA nas DUAS bordas: o W
        // gravado no reservoir depende do modo, e um W inflado do modo anterior sobreviveria
        // varios frames realimentando o wSum — o A/B compararia historico contaminado.
        void SetTemporalBiasCorrection(bool V) {
            if (V != TemporalBiasCorr) NeedsClear = true;
            TemporalBiasCorr = V;
        }
        bool GetTemporalBiasCorrection() const { return TemporalBiasCorr; }
        // Muda quais amostras sao ACEITAS no reuso, entao o que ja esta gravado foi montado sob
        // a outra regra: invalida nas duas bordas, igual a correcao de vies.
        void SetJacobianKillBackface(bool V) {
            if (V != JacobianKillBackface) NeedsClear = true;
            JacobianKillBackface = V;
        }
        bool GetJacobianKillBackface() const { return JacobianKillBackface; }

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void CreateConstantBuffer(ID3D12Device* Device);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FComputePipeline TracePSO;   // 14 SRV, 3 UAV, heap-directly-indexed (Pass A)
        // Gemea instrumentada do Pass A: mesmas tabelas, mas com o slot falso da NVAPI no root
        // sig e o timer no shader. PSO separada e nao um if no CB porque a instrumentacao custa
        // registrador — o passe normal nao pode pagar por um recurso de debug.
        FComputePipeline TracePSOTimed;
        FComputePipeline SpatialPSO; // 10 SRV, 1 UAV, heap-directly-indexed (Pass B; alpha-test M6)
        FComputePipeline NrdPackPSO; // 4 SRV [GITex,gbuf,depth,vel], 4 UAV [NRD IN] (Fase C)

        Microsoft::WRL::ComPtr<ID3D12Resource> GITexture;
        D3D12_RESOURCE_STATES GITextureState = D3D12_RESOURCE_STATE_COMMON;
        // Reservoir ping-pong em DUAS texturas (eram quatro): 32 B/pixel no lugar de 48.
        //   Res0 = RGBA32F      [x2.xyz, W]
        //   Res1 = RGBA32_UINT  [n2 oct | Lo RGB9E5 | M+idade | n1 oct]
        // O x1 saiu: vem do depth (espacial) e do historico de superficie (temporal). Ver o
        // cabecalho de empacotamento em Shaders/GI/ReSTIRReservoir.hlsli.
        Microsoft::WRL::ComPtr<ID3D12Resource> Res0[2], Res1[2];
        D3D12_RESOURCE_STATES Res0State[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES Res1State[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 GITexSRV   = kInvalidSlot;
        u32 GITexUAV   = kInvalidSlot;
        u32 Res0SRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 Res1SRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 Res0UAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 Res1UAV[2] = { kInvalidSlot, kInvalidSlot };
        // Indexadas pela PARIDADE do ping-pong (FrameParity), nao pelo FrameSlot do frame em voo:
        // o conteudo da tabela depende de qual conjunto de reservoirs e prev e qual e curr.
        // TraceTable[p]    = 14 SRVs [TLAS,sky,inst,irrad,dist,probe,depth,gbuf,vel,prev0,prev1,
        //                             filler,filler,luzes] (prev = Res*[1-p]). Os fillers
        //                    sobrou do reservoir de 4 texturas e fica de proposito: o t13 das
        //                    luzes e reescrito por frame num offset FIXO (SetPunctualLightsSRV),
        //                    entao encolher a tabela mudaria o registrador delas.
        // TraceUAVTable[p] = 3 UAVs  [GITex, curr0, curr1].
        // SpatialTable[p]  = 10 SRVs [TLAS, curr0, curr1, filler, filler, gbuf, depth, inst×3].
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
        FDDGICascadeConstants GICascadesCPU{};
        f32  GIMaxRayDist = 0.0f;

        u32  Width = 0, Height = 0;
        u32  FrameParity = 0;
        u32  TimerSlot   = kInvalidSlot; // alvo de timer vigente (kInvalidSlot = captura off)
        bool TraceTimed  = false;        // permutacao instrumentada criada com sucesso
        bool NeedsClear  = false;
        bool Initialized = false;
        bool Ready       = false;

        // Tunaveis.
        f32  AlbedoLOD      = 2.0f;
        bool Temporal       = true;
        bool Spatial        = true;   // reuso espacial (off = só temporal = A2)
        bool UseNrd         = false;  // denoise via NRD RELAX (Fase C); off = ReSTIR cru no deferred
        bool FoliageShadows = true;   // folhagem nos shadow rays do hit (mask GATHER vs OPAQUE)
        bool BackfacePolicy = false;  // retrace + terminacao preta no verso one-sided (ver setter)
        // Permutation sampling (RTXDI) no fetch temporal: SEMPRE ligado, sem knob. Ver o bloco de
        // comentario no ReSTIRGITrace.cs.hlsl (inclui o ponto de atencao sobre folhagem).
        bool Visibility     = false;  // visibility rays no espacial: shading visibility (1 raio) +
                                      // visibilidade nos pesos MIS da correcao de bias (ate K raios).
                                      // Off por padrao (custo); toggle no editor p/ A/B
        f32  MCap           = 20.0f;
        // Corte de outlier RELATIVO a vizinhanca, no fim do Pass A (ver o bloco no shader). A
        // RTXDI liga por default no GI com 0.2 (RTXDI_BoilingFilterParameters), mas aqui nasce
        // DESLIGADO: entrou junto com a correcao de vies do temporal e o A/B de 2026-08-07 nao
        // conseguiu separar os dois (o firefly que ele deveria cortar era alimentado por ela).
        // Fica pronto p/ ser medido sozinho, contra o temporal ja no 1/M.
        f32  BoilingStrength = 0.0f;
        // Correcao de vies do reuso temporal (RTXDI Basic). OFF: fiel a RTXDI, mas instavel sem a
        // realimentacao espacial que amortece o pico la — ver o bloco longo no ReSTIRGITrace.
        bool TemporalBiasCorr = false;
        // saturate() nos cossenos do Jacobiano (RTXDI/Lumen) vs o abs() historico. De volta a ON:
        // o run de baseline de 2026-08-07 saiu com o firefly INTACTO, entao nada desta sessao o
        // causa, e esta correcao (que so REJEITA amostra, nunca acrescenta energia) nao tem por
        // que continuar desligada.
        bool JacobianKillBackface = true;
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
        FGIHitSampling     GIHit;
        FReGIRShaderParams ReGIRParams{};
        FRadianceCacheShaderParams RadianceCacheParams{};
        Vec4               SkyLutParams{};
    };
}
