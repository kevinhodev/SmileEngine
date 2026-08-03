#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/GIHitSampling.h"
#include "Smile/Graphics/ReGIR.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>
#include <cstddef>

namespace Smile {
    class FTextureSRVHeap;
    class FCommandQueue;
    class FScene;
    class FGpuMesh;

    struct alignas(256) DDGIConstants {
        Vec4 GridMinSpacing;  // xyz = origem do grid (mundo), w = espacamento
        Vec4 GridCountRays;   // xyz = nº de probes por eixo, w = raios por probe
        Vec4 AtlasParams;     // x = tile size, y = atlasW, z = atlasH, w = numProbes
        Vec4 SunDirIntensity; // xyz = direcao P/ o sol, w = intensidade do sol
        Vec4 SunColorHyst;    // rgb = cor do sol, w = hysteresis (blend temporal)
        Vec4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = shadowRayBias
                              // (raios do DDGI partem de probes; o bias so desloca sombras no hit)
        Vec4 DistAtlasParams; // x = dist tile, y = dist atlasW, z = dist atlasH, w = realHitShading
        Vec4 MiscParams;      // x = relocationEnabled (Fase 2), y = deactivThresh, z = maxRays, w = minRays
        Vec4 MiscParams2;     // x = canMarkActivated (relocacao tem +1 frame agendado), y = nº luzes (F5), z = ShadowRayMask, w = -
        // Perfil de epsilons (FRayEpsilonProfile), anexado no FIM p/ nao deslocar offsets. O DDGI
        // so usa a familia (2) — os raios dele partem de probes, nao do G-buffer.
        Vec4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
        Vec4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
        // Gather do 2o bounce no hit (contrato do HitShading.hlsli). Duplica tile/W/H que ja
        // estao no DistAtlasParams porque o contrato e por NOME e vale para os tres passes.
        Vec4 GIDistParams;    // x=distTile, y=distAtlasW, z=distAtlasH, w=skipMode
        Vec4 GIBiasParams;    // x=escala do bias de superficie, y=teto em metros, zw=-
        Vec4 ReGIRGridMinSlots;
        Vec4 ReGIRInvCellEnabled;
        Vec4 ReGIRGridCountSamples;
        Vec4 ReGIRResources;
        // Parameterizacao do sky-view LUT p/ o ShadeSky do HitShading.hlsli, vinda do
        // FAtmosphere (fonte unica). Anexado no FIM p/ nao deslocar offset nenhum.
        Vec4 SkyParams;       // x = view height (km), y = raio do planeta (km), zw = livres
    };
    static_assert(offsetof(DDGIConstants, ReGIRGridMinSlots) == 208,
                  "DDGIConstants divergiu do cbuffer DDGICB");
    static_assert(offsetof(DDGIConstants, SkyParams) == 272,
                  "SkyParams deve permanecer anexado ao fim do DDGICB");

    class FDDGI {
    public:
        static constexpr int kRaysPerProbe = 64; 
        static constexpr int kTileSize     = 6;  
        static constexpr int kDistTileSize = 14; 

        void Initialize(ID3D12Device* Device);

        void SetupForScene(ID3D12Device* Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap,
                           const FScene& Scene, const Vec3& AABBMin, const Vec3& AABBMax,
                           u32 TlasSRVSlot, u32 SkyViewSRVSlot);

        // Re-upload do snapshot de materiais que TODO o RT le (DDGI, ReSTIR, reflexoes). Chamar
        // quando uma propriedade de material que o RT enxerga muda em runtime (AlphaTest,
        // TwoSided, emissivo...): o snapshot e criado uma vez no SetupForScene e nao acompanha a
        // edicao. O CHAMADOR precisa garantir GPU ociosa — e um upload heap sem versao por frame.
        void RefreshInstanceGeo(const FScene& Scene);

        void UpdatePerFrame(u32 FrameSlot, const Vec3& DirToSun, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, u32 PunctualLightCount = 0);

        // F5: copia o SRV do buffer de luzes puntuais do frame (slot de staging do Renderer)
        // pro t8 da tabela de trace DO FrameSlot (tabela versionada por frame em voo — a do
        // frame anterior ainda pode estar sendo lida pela GPU).
        void SetPunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  u32 StagingSlot, u32 FrameSlot);

        // Async compute (F3): transicoes envolvendo PIXEL_SHADER_RESOURCE nao podem ser
        // gravadas em fila COMPUTE. TransitionForUpdate (atlases/trace -> UAV) vai na
        // fila DIRETA antes do signal; RecordUpdate (dispatches + transicoes UAV/NON_PIXEL,
        // compute-legais) roda em qualquer fila; TransitionForRead (atlases -> PIXEL|
        // NON_PIXEL) vai na direta depois do wait. No caminho sincrono, chamar as tres em
        // sequencia na mesma list = comportamento antigo.
        void TransitionForUpdate(ID3D12GraphicsCommandList* CommandList);
        void RecordUpdate(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);
        void TransitionForRead(ID3D12GraphicsCommandList* CommandList);

        // Relocation de probes (transiente pos-setup) transiciona ProbeData no MEIO do
        // update a partir de estado com PIXEL — nesses frames o update roda sincrono.
        bool CanRunAsync() const { return RelocateFramesLeft == 0; }

        bool IsReady() const { return Ready; }
        u32  IrradianceAtlasSRV() const { return AtlasSRVSlot; }
        u32  InstanceSRV() const { return InstanceSRVSlot; }
        u32  SceneGITableStart()  const { return SceneGITableStart_; }
        u32  DistAtlasSRV()    const { return DistSRVSlot; }
        u32  ProbesTraceSRV()  const { return ProbesTraceSRVSlot; }
        u32  ProbeDataSRV()    const { return ProbeDataSRVSlot; }
        u32  ProbeRayCountSRV()const { return ProbeRayCountSRVSlot; } 
        ID3D12Resource* IrradianceAtlasResource() const { return IrradAtlas.Get(); }
        ID3D12Resource* DistanceAtlasResource() const   { return DistAtlas.Get(); }
        u32  NumProbesCount()  const { return NumProbes; }
        u32  RaysPerProbe()    const { return kRaysPerProbe; }

        Vec3 GridMin()   const { return GridMinV; }
        f32  Spacing()   const { return SpacingV; }
        Vec3 GridCount() const { return Vec3{ (f32)CountX, (f32)CountY, (f32)CountZ }; }
        f32  AtlasW()    const { return (f32)AtlasWidth; }
        f32  AtlasH()    const { return (f32)AtlasHeight; }
        f32  TileSizeF() const { return (f32)kTileSize; }
        f32  DistAtlasW()    const { return (f32)DistAtlasWidth; }
        f32  DistAtlasH()    const { return (f32)DistAtlasHeight; }
        f32  DistTileSizeF() const { return (f32)kDistTileSize; }
        f32  MaxRayDistance() const { return MaxRayDist; } 
        // O atlas de distancia guarda os dois momentos ja limitados a esta vizinhanca.
        // Nao confundir com MaxRayDistance(), que e o alcance do trace na cena inteira.
        f32  DistanceMomentMax() const { return SpacingV * 2.6f; }
        // Ordem fisica dos tiles no atlas: X varia primeiro, depois Z; Y ocupa as linhas.
        // ProbeLinear, usado pelos buffers, varia X, depois Y, depois Z.
        u32  AtlasTileFromProbe(u32 ProbeIndex) const {
            if (ProbeIndex >= NumProbes || CountX <= 0 || CountY <= 0) return 0;
            const u32 XY = static_cast<u32>(CountX * CountY);
            const u32 Z  = ProbeIndex / XY;
            const u32 R  = ProbeIndex - Z * XY;
            const u32 Y  = R / static_cast<u32>(CountX);
            const u32 X  = R - Y * static_cast<u32>(CountX);
            return X + Z * static_cast<u32>(CountX)
                     + Y * static_cast<u32>(CountX * CountZ);
        }

        void SetIntensity(f32 V)  { Intensity = V; }
        f32  GetIntensity() const { return Intensity; }
        // Perfil compartilhado de epsilons (dono = Renderer, empurra todo frame).
        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        // Gather do 2o bounce (dono = Renderer, empurra todo frame; ver FGIHitSampling).
        void SetGIHitSampling(const FGIHitSampling& S) { GIHit = S; }
        void SetReGIRParams(const FReGIRShaderParams& P) { ReGIRParams = P; }
        // Parameterizacao do sky-view LUT p/ o ShadeSky dos raios que escapam (dono = Renderer,
        // empurra todo frame a partir do FAtmosphere — fonte unica, ver Atmosphere.h).
        void SetSkyParams(f32 ViewHeightKm, f32 BottomRadiusKm) {
            SkyLutParams = { ViewHeightKm, BottomRadiusKm, 0.0f, 0.0f };
        }

        // Reset one-shot do atlas: o proximo update que REALMENTE rodar usa histerese 0, ou seja,
        // substitui o conteudo em vez de misturar. Necessario p/ calibracao: com Hysteresis 0.99
        // o atlas guarda 99% do resultado por update, entao um knob de epsilon pareceria inerte
        // por dezenas de frames. O flag e consumido no RecordUpdate, nao aqui — se o passe nao
        // rodar neste frame, o reset continua pendente.
        void ResetHistoryOnce()   { HysteresisResetPending = true; }

        void SetHysteresis(f32 V) { Hysteresis = V; }
        f32  GetHysteresis() const{ return Hysteresis; }

        void SetDeactivationThreshold(f32 V) { DeactivationThreshold = V; TriggerReclassify(); }
        f32  GetDeactivationThreshold() const { return DeactivationThreshold; }

        void SetAdaptiveRays(bool V) { AdaptiveRays = V; TriggerReclassify(); }
        bool GetAdaptiveRays() const { return AdaptiveRays; }
        void SetMaxRays(int V) { MaxRays = V; TriggerReclassify(); }   
        int  GetMaxRays() const { return MaxRays; }
        void SetMinRays(int V) { MinRays = V; TriggerReclassify(); }   
        int  GetMinRays() const { return MinRays; }

        void SetRealHitShading(bool V) { RealHitShading = V; }
        bool GetRealHitShading() const { return RealHitShading; }

        // Folhagem nos shadow rays do hit (ON = alpha-test por candidato; OFF = mask so-opaco).
        void SetFoliageShadows(bool V) { FoliageShadows = V; }
        bool GetFoliageShadows() const { return FoliageShadows; }

        void SetRelocation(bool V) { Relocation = V; RelocateFramesLeft = V ? kRelocateConvergeFrames : 4; }
        bool GetRelocation() const { return Relocation; }

        // Amostragem (nao afeta o atlas — sao knobs do SAMPLER, lidos pelo deferred/forward e
        // pelo diagnostico pontual). Ficam aqui, e nao no FRayEpsilonProfile, porque nao sao
        // epsilons de RAIO: o perfil descreve a geometria dos raios de GI/reflexo/sombra e sua
        // troca limpa reservoirs e denoiser, o que nao faz sentido para um offset de leitura.
        //
        // Teto do bias em METROS. 0 = sem teto = comportamento historico (bias = 0.75*spacing*
        // scale, formula do Flax). Existe porque o espacamento do grid aqui vem da AABB da cena
        // inteira: com spacing de 8 m o bias historico vale 1,20 m e o ponto de amostragem
        // atravessa parede. O RTXGI moderno resolve com normalBias/viewBias absolutos; o teto e
        // a versao barata, que preserva cena pequena e corta cena grande.
        void SetSurfaceBiasMax(f32 V)   { SurfaceBiasMax = V < 0.0f ? 0.0f : V; }
        f32  GetSurfaceBiasMax() const  { return SurfaceBiasMax; }
        void SetSurfaceBiasScale(f32 V) { SurfaceBiasScale = V; }
        f32  GetSurfaceBiasScale() const{ return SurfaceBiasScale; }

        // O peso de backface e sempre medido da posicao SEM bias (o que o Flax faz,
        // DDGI.hlsl:210-215). Teve toggle enquanto era hipotese; virou comportamento fixo depois
        // do A/B — nao e trade-off, e correcao, e o modo antigo so serviria p/ reintroduzir o bug.

        // Largura, EM CELULAS, do fade para o ambiente hemisferico nas bordas do volume.
        // 0 = desligado (o gather clampa e estende as probes de borda ao infinito, que e o
        // comportamento historico). O terreno fica fora do volume de proposito, entao sem isto
        // ele inteiro herda a irradiancia da ultima fileira de probes.
        void SetVolumeFadeProbes(f32 V) { VolumeFadeProbes = V < 0.0f ? 0.0f : V; }
        f32  GetVolumeFadeProbes() const { return VolumeFadeProbes; }

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseSceneResources(FTextureSRVHeap& SRVHeap);

        void TriggerReclassify() {
            if (Relocation && Ready && RelocateFramesLeft < kReclassifyFrames)
                RelocateFramesLeft = kReclassifyFrames;
        }
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return CB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(DDGIConstants);
        }

        FVolumetricPipeline TracePSO;      
        FVolumetricPipeline UpdatePSO;     
        FVolumetricPipeline UpdateDistPSO; 
        FVolumetricPipeline RelocatePSO;   

        Microsoft::WRL::ComPtr<ID3D12Resource> IrradAtlas;       
        Microsoft::WRL::ComPtr<ID3D12Resource> DistAtlas;        
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbesTrace;      
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceGeoBuf;
        u32                                    InstanceGeoCount = 0; // capacidade do snapshot acima
        // Definido no .cpp (DDGIInstanceGeo e local daquele arquivo); _Mapped tem _Count entradas.
        void FillInstanceGeo(const FScene& Scene, u8* Mapped, u32 Count) const;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeDataBuf;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeRayCountBuf; 

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 AtlasSRVSlot       = kInvalidSlot;
        u32 AtlasUAVSlot       = kInvalidSlot;
        u32 DistSRVSlot        = kInvalidSlot;
        u32 DistUAVSlot        = kInvalidSlot;
        u32 ProbesTraceSRVSlot = kInvalidSlot;
        u32 ProbesTraceUAVSlot = kInvalidSlot;
        u32 InstanceSRVSlot    = kInvalidSlot;
        // SRVs bindless de VB/IB por mesh único (2 slots contíguos por mesh: base+2i = VB,
        // base+2i+1 = IB) — o InstanceGeo carrega os índices; substitui os merged buffers.
        u32 MeshGeoSlotBase    = kInvalidSlot;
        u32 MeshGeoSlotCount   = 0;
        // Mesh -> slot bindless do VB (IB = +1). Era local do SetupForScene; virou membro porque
        // o RefreshInstanceGeo precisa remontar o snapshot sem refazer a alocacao de descriptors.
        std::unordered_map<const FGpuMesh*, u32> MeshGeoSlot;
        u32 ProbeDataSRVSlot   = kInvalidSlot;
        u32 ProbeDataUAVSlot   = kInvalidSlot;
        u32 ProbeRayCountSRVSlot = kInvalidSlot;
        u32 ProbeRayCountUAVSlot = kInvalidSlot;
        // Tabela do trace versionada por frame em voo: o t8 (luzes) e reescrito todo frame e o
        // frame anterior ainda pode estar lendo a tabela dele no heap shader-visible.
        static constexpr u32 kTraceTables = 2; // == FCommandQueue::kFramesInFlight (assert no .cpp)
        u32 TraceTable[kTraceTables] = { kInvalidSlot, kInvalidSlot };
        u32 SceneGITableStart_ = kInvalidSlot;
        u32 UpdateTableStart   = kInvalidSlot;   // [ProbesTrace, ProbeData] p/ Update/UpdateDist

        D3D12_RESOURCE_STATES AtlasState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES DistState      = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbesState    = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB   = nullptr;
        u32 FrameSlot  = 0;
        DDGIConstants CPU{};

        Vec3 GridMinV{ 0,0,0 };
        f32  SpacingV    = 1.0f;
        int  CountX = 0, CountY = 0, CountZ = 0;
        u32  NumProbes   = 0;
        u32  AtlasWidth  = 0, AtlasHeight = 0;
        u32  DistAtlasWidth = 0, DistAtlasHeight = 0;

        static constexpr D3D12_RESOURCE_STATES kAtlasRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        f32  Intensity    = 1.0f;
        f32  Hysteresis   = 0.99f;
        bool HysteresisResetPending = false; // ver ResetHistoryOnce
        f32  SkyIntensity = 1.0f;
        // NormalBias saiu daqui: o bias dos shadow rays do 2o hit e o mesmo p/ ReSTIR, reflexoes e
        // DDGI (o nome era historico), entao vive no perfil compartilhado — sem isso o sweep de
        // calibracao deixaria o DDGI em 20 cm enquanto os outros descem.
        FRayEpsilonProfile RayEps; // perfil compartilhado (dono = Renderer)
        FGIHitSampling     GIHit;
        FReGIRShaderParams ReGIRParams{};
        Vec4               SkyLutParams{};
        f32  MaxRayDist   = 0.0f;
        bool RealHitShading = true;
        bool FoliageShadows = true; // sombra de folhagem nos shadow rays do GI (GATHER vs OPAQUE)
        bool Relocation     = true; 
        f32  DeactivationThreshold = 0.20f; 
        bool AdaptiveRays   = false;
        int  MaxRays        = 64;
        int  MinRays        = 16;
        f32  SurfaceBiasScale = 0.2f;  // o `bias` do GetDDGISurfaceBias do Flax
        // Defaults LIGADOS (o legado seria 0.0f / false). Com o grid dimensionado pela AABB da
        // cena, o bias sem teto vale 1,20 m no Bistro — o ponto de amostragem atravessa parede.
        f32  SurfaceBiasMax   = 0.40f; // metros; 0 = sem teto (comportamento historico)
        f32  VolumeFadeProbes = 1.0f;  // celulas de fade na borda; 0 = sem fallback (historico)
        static constexpr u32 kRelocateConvergeFrames = 180;
        static constexpr u32 kReclassifyFrames = 6;
        u32  RelocateFramesLeft = 0;

        bool Initialized = false;
        bool Ready       = false;
    };
}
