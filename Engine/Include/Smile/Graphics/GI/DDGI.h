#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/RayTracing/RayEpsilons.h"
#include "Smile/Graphics/GI/GIHitSampling.h"
#include "Smile/Graphics/GI/ReGIR.h"
#include "Smile/Graphics/GI/RadianceCache.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstddef>

namespace Smile {
    class FTextureSRVHeap;
    class FCommandQueue;
    class FScene;

    // Mudancas de geometria tambem invalidam a classificacao das sondas.
    enum class EGIRegionChange {
        Radiometric,
        Geometry
    };

    // Layout de cascatas compartilhado por C++/HLSL; a ordem integra a ABI do cbuffer.
    struct alignas(16) FDDGICascadeConstants {
        // xy = cascatas/sondas por cascata; zw = sondas agendadas/intervalo da grossa.
        Vec4 Params{ 1.0f, 0.0f, 0.0f, 1.0f };
        Vec4 GridMinSpacing[4]{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f },
                                { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
        // Offset toroidal em celulas; explicito porque o GridMin grosso nao alinha à grade.
        Vec4 ScrollOffset[4]{};
    };
    static_assert(sizeof(FDDGICascadeConstants) == 144,
                  "o bloco de cascatas e copiado campo-a-campo para cinco cbuffers");
    static_assert(alignof(FDDGICascadeConstants) == 16,
                  "o bloco entra em cbuffer: alinhamento de float4");
    static_assert(offsetof(FDDGICascadeConstants, GridMinSpacing) == 16,
                  "o array tem de seguir imediatamente o Params, sem padding");
    static_assert(offsetof(FDDGICascadeConstants, ScrollOffset) == 80,
                  "o scroll segue imediatamente o GridMinSpacing, sem padding");

    struct alignas(256) DDGIConstants {
        Vec4 GridMinSpacing;  // xyz = origem do grid (mundo), w = espacamento
        Vec4 GridCountRays;   // xyz = nº de probes por eixo, w = raios por probe
        Vec4 AtlasParams;     // x = tile size, y = atlasW, z = atlasH, w = numProbes
        Vec4 SunDirIntensity; // xyz = direcao P/ o sol, w = intensidade do sol
        Vec4 SunColorHyst;    // rgb = cor do sol, w = hysteresis (blend temporal)
        Vec4 TraceParams;     // x = frameIndex, y = maxRayDist, z = skyIntensity, w = shadowRayBias
                              // (raios do DDGI partem de probes; o bias so desloca sombras no hit)
        Vec4 DistAtlasParams; // x = dist tile, y = dist atlasW, z = dist atlasH,
                              // w = hysteresis do atlas de DISTANCIA (propria; ver kDistHysteresis)
        Vec4 MiscParams;      // x = relocationEnabled, y = deactivThresh, z = maxRays, w = minRays
        Vec4 MiscParams2;     // x = canMarkActivated (relocacao tem +1 frame agendado), y = nº luzes (F5),
                              // z = ShadowRayMask, w = detector de histerese adaptativa (so a irradiancia le)
        // Campos abaixo sao apenas anexados: varios shaders consomem o layout por offset.
        Vec4 RayEpsA;         // x=originFloorMin, y=originFloorPerMeter, z=angularMax, w=shadowRayBiasMin
        Vec4 RayEpsB;         // x=shadowRayTMin, y=visRayTMin, z=visRayEndMargin, w=angularMinRatio
        Vec4 GIDistParams;    // x=distTile, y=distAtlasW, z=distAtlasH, w=skipMode
        Vec4 GIBiasParams;    // x=escala do bias de superficie, y=teto em metros, zw=-
        Vec4 ReGIRGridMinSlots;
        Vec4 ReGIRInvCellEnabled;
        Vec4 ReGIRGridCountSamples;
        Vec4 ReGIRResources;
        Vec4 SkyParams;       // x = view height (km), y = raio do planeta (km), zw = livres
        Vec4 InvalidateMin;      // xyz = min da caixa, w = 1 se ha invalidacao ativa
        Vec4 InvalidateMaxHyst;  // xyz = max da caixa, w = hysteresis a usar dentro dela
        Vec4 RadianceCacheCamCell;
        Vec4 RadianceCacheLodCapFlags;
        Vec4 RadianceCacheResources;
        Vec4 MiscParams3;     // x = hysteresis regional do dist atlas, y = 1 se a janela dele
                              // esta aberta, z = 1 se o passe de relocacao/classificacao roda
                              // neste frame (o TRACE le p/ nao decimar; ver DDGITrace), w = livre
        FDDGICascadeConstants Cascades;
        // xyz = delta inteiro desde o ultimo update executado da cascata; w = houve mudanca.
        Vec4 CascadeScrollDelta[4];
        // x = lista compacta de sondas ativas habilitada neste update.
        Vec4 ProbeCompactionParams;
    };
    static_assert(offsetof(DDGIConstants, CascadeScrollDelta) == 528,
                  "o delta de scroll segue o bloco de cascatas, no fim do DDGICB");
    static_assert(offsetof(DDGIConstants, ReGIRGridMinSlots) == 208,
                  "DDGIConstants divergiu do cbuffer DDGICB");
    static_assert(offsetof(DDGIConstants, SkyParams) == 272,
                  "SkyParams deve permanecer anexado ao fim do DDGICB");
    static_assert(offsetof(DDGIConstants, InvalidateMin) == 288,
                  "InvalidateMin/MaxHyst devem permanecer anexados ao fim do DDGICB");
    static_assert(offsetof(DDGIConstants, MiscParams3) == 368,
                  "MiscParams3 deve permanecer anexado ao fim do DDGICB");
    static_assert(offsetof(DDGIConstants, Cascades) == 384,
                  "o bloco de cascatas deve permanecer anexado ao fim do DDGICB");
    static_assert(offsetof(DDGIConstants, ProbeCompactionParams) == 592,
                  "a compactacao deve permanecer anexada depois dos deltas de scroll");

    // Menor numero de updates que deixa no maximo Residual do historico.
    constexpr u32 DDGIFramesForResidual(f32 _H, f32 _Residual) {
        u32 N = 0; f32 V = 1.0f;
        while (V > _Residual && N < 512u) { V *= _H; ++N; }
        return N;
    }

    class FDDGI : public FRenderPass {
    public:
        const char* Name() const override { return "DDGI"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;
        EHistoryTarget HistoryTargets() const override { return EHistoryTarget::DDGIAtlas; }
        void OnInvalidateHistory(EHistoryTarget) override { ResetHistoryOnce(); }

        static constexpr int kRaysPerProbe = 64;
        static constexpr int kTileSize     = 6;
        static constexpr int kDistTileSize = 14;
        // A cascata 0 e a mais fina; a ultima cobre a cena.
        static constexpr u32 kMaxCascades = 4;
        // Espelhado por DDGI_TRACE_PROBES_PER_ROW.
        static constexpr u32 kTraceProbesPerRow = 256;
        static_assert(kTraceProbesPerRow * kRaysPerProbe <= 16384,
                      "a linha do ProbesTrace nao pode passar da largura maxima de Texture2D; "
                      "mudar aqui exige mudar DDGI_TRACE_PROBES_PER_ROW junto");

        // Espelhado por DDGI_DISPATCH_GROUPS_X; limita as dimensoes do dispatch no D3D12.
        static constexpr u32 kDispatchGroupsX = 1024;
        static_assert(kDispatchGroupsX <= 65535,
                      "a largura da grade de grupos e ela propria um Dispatch");
        static u32 DispatchGroupsX(u32 Probes) {
            const u32 Rows = std::max((Probes + kDispatchGroupsX - 1) / kDispatchGroupsX, 1u);
            return std::max((Probes + Rows - 1) / Rows, 1u);
        }
        static u32 DispatchGroupsY(u32 Probes) {
            const u32 X = DispatchGroupsX(Probes);
            return (Probes + X - 1) / X;
        }

        void Initialize(ID3D12Device* Device);

        void SetupForScene(ID3D12Device* Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap,
                           const FScene& Scene, const Vec3& AABBMin, const Vec3& AABBMax,
                           u32 TlasSRVSlot, u32 SkyViewSRVSlot, u32 InstanceGeoSRVSlot);

        // Deve rodar antes de qualquer consumidor do frame capturar CascadeConstants().
        void PrepareCascadePlacement(const Vec3& CameraPos);

        void UpdatePerFrame(u32 FrameSlot, const Vec3& DirToSun, f32 SunIntensity,
                            const Vec3& SunColor, u32 FrameIndex, u32 PunctualLightCount = 0);

        void SetPunctualLightsSRV(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                  u32 StagingSlot, u32 FrameSlot);

        // A fila direta faz as transicoes PIXEL_SHADER_RESOURCE em torno do update assincrono.
        void TransitionForUpdate(ID3D12GraphicsCommandList* CommandList);
        void RecordUpdate(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);
        void TransitionForRead(ID3D12GraphicsCommandList* CommandList);

        bool CanRunAsync() const { return true; }

        bool IsReady() const { return Ready; }
        u32  IrradianceAtlasSRV() const { return AtlasSRVSlot; }
        u32  SceneGITableStart()  const { return SceneGITableStart_; }
        u32  DistAtlasSRV()    const { return DistSRVSlot; }
        u32  ProbesTraceSRV()  const { return ProbesTraceSRVSlot; }
        u32  ProbeDataSRV()    const { return ProbeDataSRVSlot; }
        u32  ProbeRayCountSRV()const { return ProbeRayCountSRVSlot; } 
        ID3D12Resource* IrradianceAtlasResource() const { return IrradAtlas.Get(); }
        ID3D12Resource* DistanceAtlasResource() const   { return DistAtlas.Get(); }
        u32  NumProbesCount()  const { return NumProbes; }
        u32  ProbesPerCascade()const { return ProbesPerCascade_; }
        u32  CascadeCount()    const { return CascadeCount_; }
        u32  RaysPerProbe()    const { return kRaysPerProbe; }
        // Com duas cascatas, atualiza a fina sempre e a grossa em frames alternados.
        void SetInterleavedUpdates(bool V) {
            if (InterleavedUpdates == V) return;
            InterleavedUpdates = V;
            CoarseDue_ = true;
        }
        bool GetInterleavedUpdates() const { return InterleavedUpdates; }
        u32  ScheduledCascadeCount() const { return ScheduledCascadeCount_; }
        u32  LastUpdatedCascadeCount() const { return LastUpdatedCascadeCount_; }
        u64  UpdateSerial() const { return UpdateSerial_; }
        u64  LastForcedUpdateSerial() const { return LastForcedUpdateSerial_; }
        bool ScheduledFullWasForced() const { return ScheduledFullForced_; }
        u32  CascadeUpdateAge(u32 C) const {
            return CascadeUpdateAge_[C < kMaxCascades ? C : 0];
        }

        // A lista compacta inclui sondas ativas e laminas recem-expostas pelo scroll.
        void SetProbeCompaction(bool V) {
            if (ProbeCompaction == V) return;
            ProbeCompaction = V;
            LastProbeWakeSerial_ = UpdateSerial_;
            if (V) TriggerReclassify();
        }
        bool GetProbeCompaction() const { return ProbeCompaction; }
        bool ProbeCompactionScheduled() const { return ProbeCompactionThisUpdate_; }
        bool LastUpdateUsedProbeCompaction() const { return LastUpdateUsedProbeCompaction_; }
        u32  LastActiveProbeCount() const { return LastActiveProbeCount_; }
        u32  LastCompactedProbeCapacity() const { return LastCompactedProbeCapacity_; }
        static constexpr u32 ProbeWakeInterval() { return kProbeWakeInterval; }
        u64  LastProbeWakeSerial() const { return LastProbeWakeSerial_; }

        // Os acessores legados GridMin/Spacing apontam para a cascata grossa da cena.
        u32  CoarseCascade() const { return CascadeCount_ > 0 ? CascadeCount_ - 1 : 0; }
        Vec3 GridMin()   const { return Cascades[CoarseCascade()].GridMin; }
        f32  Spacing()   const { return Cascades[CoarseCascade()].Spacing; }
        Vec3 CascadeGridMin(u32 C) const { return Cascades[C < kMaxCascades ? C : 0].GridMin; }
        f32  CascadeSpacing(u32 C) const { return Cascades[C < kMaxCascades ? C : 0].Spacing; }
        int  CascadeScroll(u32 C, int Axis) const {
            const FCascade& Cs = Cascades[C < kMaxCascades ? C : 0];
            return (Axis >= 0 && Axis < 3) ? Cs.Scroll[Axis] : 0;
        }

        // Entradas sem uso repetem a cascata grossa para manter leituras acidentais definidas.
        FDDGICascadeConstants CascadeConstants() const {
            FDDGICascadeConstants C{};
            C.Params = { static_cast<f32>(CascadeCount_),
                         static_cast<f32>(ProbesPerCascade_), 0.0f, 1.0f };
            for (u32 i = 0; i < kMaxCascades; ++i) {
                const FCascade& Cs = Cascades[i < CascadeCount_ ? i : CoarseCascade()];
                C.GridMinSpacing[i] = { Cs.GridMin.X, Cs.GridMin.Y, Cs.GridMin.Z, Cs.Spacing };
                C.ScrollOffset[i]   = { static_cast<f32>(Cs.Scroll[0]),
                                        static_cast<f32>(Cs.Scroll[1]),
                                        static_cast<f32>(Cs.Scroll[2]), 0.0f };
            }
            return C;
        }
        Vec3 GridCount() const { return Vec3{ (f32)CountX, (f32)CountY, (f32)CountZ }; }
        f32  AtlasW()    const { return (f32)AtlasWidth; }
        f32  AtlasH()    const { return (f32)AtlasHeight; }
        f32  TileSizeF() const { return (f32)kTileSize; }
        f32  DistAtlasW()    const { return (f32)DistAtlasWidth; }
        f32  DistAtlasH()    const { return (f32)DistAtlasHeight; }
        f32  DistTileSizeF() const { return (f32)kDistTileSize; }
        f32  MaxRayDistance() const { return MaxRayDist; } 
        // O debug global normaliza pela grossa; a inspecao individual usa o metodo abaixo.
        f32  DistanceMomentMax() const { return Spacing() * 2.6f; }
        f32  CascadeDistanceMomentMax(u32 C) const { return CascadeSpacing(C) * 2.6f; }
        // Mapeamentos inversos do atlas em bandas, espelhados por DDGI_TileOrigin.
        u32  AtlasTileFromProbe(u32 ProbeIndex) const {
            if (ProbeIndex >= NumProbes || CountX <= 0 || CountY <= 0 || TilesPerRow == 0 ||
                ProbesPerCascade_ == 0)
                return 0;
            const u32 CX = static_cast<u32>(CountX), CY = static_cast<u32>(CountY);
            const u32 Cascade = ProbeIndex / ProbesPerCascade_;
            const u32 Local   = ProbeIndex - Cascade * ProbesPerCascade_;
            const u32 X = Local % CX;
            const u32 Y = (Local / CX) % CY;
            const u32 Z = Local / (CX * CY);
            const u32 Plane = X + Z * CX;
            const u32 Band  = Plane / TilesPerRow;
            const u32 Row   = Y + Band * CY + Cascade * TileRowsPerCascade;
            return Row * TilesPerRow + (Plane - Band * TilesPerRow);
        }
        bool ProbeFromAtlasTile(u32 Tile, u32& OutProbe) const {
            if (CountX <= 0 || CountY <= 0 || TilesPerRow == 0 || TileRowsPerCascade == 0)
                return false;
            const u32 CX = static_cast<u32>(CountX), CY = static_cast<u32>(CountY);
            const u32 RowAbs  = Tile / TilesPerRow;
            const u32 C       = Tile - RowAbs * TilesPerRow;
            const u32 Cascade = RowAbs / TileRowsPerCascade;
            const u32 Row     = RowAbs - Cascade * TileRowsPerCascade;
            const u32 Y       = Row % CY;
            const u32 Band    = Row / CY;
            const u32 Plane   = Band * TilesPerRow + C;
            if (Cascade >= CascadeCount_ || Plane >= CX * static_cast<u32>(CountZ)) return false;
            OutProbe = Cascade * ProbesPerCascade_ +
                       (Plane % CX) + Y * CX + (Plane / CX) * CX * CY;
            return OutProbe < NumProbes;
        }
        u32  AtlasTilesPerRow() const { return TilesPerRow; }
        u32  AtlasTileRows() const { return TileRowsPerCascade * CascadeCount_; }

        // Zero e a sentinela de desligado nos cbuffers consumidores.
        static constexpr f32 kIntensityMin = 1e-3f;
        void SetIntensity(f32 V)  { Intensity = V < kIntensityMin ? kIntensityMin : V; }
        f32  GetIntensity() const { return Intensity; }
        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        void SetGIHitSampling(const FGIHitSampling& S) { GIHit = S; }
        void SetReGIRParams(const FReGIRShaderParams& P) { ReGIRParams = P; }
        void SetRadianceCacheParams(const FRadianceCacheShaderParams& P) { RadianceCacheParams = P; }
        void SetSkyParams(f32 ViewHeightKm, f32 BottomRadiusKm) {
            SkyLutParams = { ViewHeightKm, BottomRadiusKm, 0.0f, 0.0f };
        }
        void SetSkyIntensity(f32 V) { SkyIntensity = V; }
        f32  GetSkyIntensity() const { return SkyIntensity; }

        // Consumido somente quando RecordUpdate realmente executa.
        void ResetHistoryOnce()   { HysteresisResetPending = true; }

        // Une as caixas e reduz temporariamente a histerese apenas nas sondas proximas.
        void InvalidateRegion(const Vec3& Min, const Vec3& Max, EGIRegionChange Change);

        static constexpr f32 kHysteresisMax = 0.98f;
        void SetHysteresis(f32 V) {
            Hysteresis = V > kHysteresisMax ? kHysteresisMax : (V < 0.0f ? 0.0f : V);
        }
        f32  GetHysteresis() const{ return Hysteresis; }

        // Rede apenas da irradiancia; desativada por padrao porque o ruido pode dispara-la.
        void SetAdaptiveHysteresis(bool V) { AdaptiveHysteresis = V; }
        bool GetAdaptiveHysteresis() const { return AdaptiveHysteresis; }

        void SetDeactivationThreshold(f32 V) { DeactivationThreshold = V; TriggerReclassify(); }
        f32  GetDeactivationThreshold() const { return DeactivationThreshold; }

        // Mudar a contagem de raios exige republica-la pelo passe de classificacao.
        void SetAdaptiveRays(bool V) { AdaptiveRays = V; TriggerReclassify(); }
        bool GetAdaptiveRays() const { return AdaptiveRays; }
        void SetMaxRays(int V) { MaxRays = ClampRays(V); TriggerReclassify(); }
        int  GetMaxRays() const { return MaxRays; }
        void SetMinRays(int V) { MinRays = ClampRays(V); TriggerReclassify(); }
        int  GetMinRays() const { return MinRays; }

        void SetFoliageShadows(bool V) { FoliageShadows = V; }
        bool GetFoliageShadows() const { return FoliageShadows; }

        void SetRelocation(bool V) { Relocation = V; RelocateFramesLeft = V ? kRelocateConvergeFrames : 4; }
        bool GetRelocation() const { return Relocation; }

        // Controles de amostragem; SurfaceBiasMax e teto absoluto no mundo (0 = sem teto).
        void SetSurfaceBiasMax(f32 V)   { SurfaceBiasMax = V < 0.0f ? 0.0f : V; }
        f32  GetSurfaceBiasMax() const  { return SurfaceBiasMax; }
        void SetSurfaceBiasScale(f32 V) { SurfaceBiasScale = V; }
        f32  GetSurfaceBiasScale() const{ return SurfaceBiasScale; }

        // Largura do fade da borda em celulas de sonda; 0 desativa.
        void SetVolumeFadeProbes(f32 V) { VolumeFadeProbes = V < 0.0f ? 0.0f : V; }
        f32  GetVolumeFadeProbes() const { return VolumeFadeProbes; }

        // Aplicado no proximo SetupForScene, pois a contagem dimensiona recursos da GPU.
        void SetDesiredCascades(u32 V) {
            DesiredCascades = V < 1 ? 1u : (V > kMaxCascades ? kMaxCascades : V);
        }
        u32  GetDesiredCascades() const { return DesiredCascades; }

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseSceneResources(FTextureSRVHeap& SRVHeap);

        // O stride do trace exige contagem de raios em potencia de dois.
        static int ClampRays(int V) {
            if (V >= kRaysPerProbe) return kRaysPerProbe;
            int R = 1;
            while (R * 2 <= V) R *= 2;
            return R;
        }

        // A classificacao escreve ProbeData e ProbeRayCount mesmo sem relocacao.
        void TriggerReclassify() {
            if (Ready && RelocateFramesLeft < kReclassifyFrames)
                RelocateFramesLeft = kReclassifyFrames;
        }
        void ScheduleUpdate();
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return CB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(DDGIConstants);
        }

        FComputePipeline TracePSO;
        FComputePipeline UpdatePSO;
        FComputePipeline UpdateDistPSO;
        FComputePipeline RelocatePSO;
        FComputePipeline CompactBuildPSO;
        FComputePipeline CompactFinalizePSO;

        Microsoft::WRL::ComPtr<ID3D12CommandSignature> DispatchCommandSignature;

        Microsoft::WRL::ComPtr<ID3D12Resource> IrradAtlas;       
        Microsoft::WRL::ComPtr<ID3D12Resource> DistAtlas;        
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbesTrace;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeDataBuf;
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeRayCountBuf; 
        Microsoft::WRL::ComPtr<ID3D12Resource> ActiveProbeIndicesBuf;
        Microsoft::WRL::ComPtr<ID3D12Resource> ActiveProbeCountBuf;
        Microsoft::WRL::ComPtr<ID3D12Resource> ActiveProbeDispatchArgsBuf;
        Microsoft::WRL::ComPtr<ID3D12Resource> ActiveProbeCountReadback;
        u8* MappedActiveProbeCount = nullptr;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32 AtlasSRVSlot       = kInvalidSlot;
        u32 AtlasUAVSlot       = kInvalidSlot;
        u32 DistSRVSlot        = kInvalidSlot;
        u32 DistUAVSlot        = kInvalidSlot;
        u32 ProbesTraceSRVSlot = kInvalidSlot;
        u32 ProbesTraceUAVSlot = kInvalidSlot;
        u32 ProbeDataSRVSlot   = kInvalidSlot;
        u32 ProbeDataUAVSlot   = kInvalidSlot;
        u32 ProbeRayCountSRVSlot = kInvalidSlot;
        u32 ProbeRayCountUAVSlot = kInvalidSlot;
        u32 ActiveProbeIndicesSRVSlot = kInvalidSlot;
        u32 ActiveProbeCountSRVSlot   = kInvalidSlot;
        u32 ActiveProbeBuildUAVStart  = kInvalidSlot; // [indices, count]
        u32 ActiveProbeIndicesUAVSlot = kInvalidSlot;
        u32 ActiveProbeCountUAVSlot   = kInvalidSlot;
        u32 ActiveProbeDispatchArgsUAVSlot = kInvalidSlot;
        // Versionada porque o SRV de luzes muda enquanto o frame anterior ainda pode le-lo.
        static constexpr u32 kTraceTables = 2; // == FCommandQueue::kFramesInFlight (assert no .cpp)
        u32 TraceTable[kTraceTables] = { kInvalidSlot, kInvalidSlot };
        u32 SceneGITableStart_ = kInvalidSlot;
        u32 UpdateTableStart   = kInvalidSlot;   // [trace, data, active indices, active count]

        D3D12_RESOURCE_STATES AtlasState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES DistState      = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbesState    = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ActiveProbeIndicesState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ActiveProbeCountState   = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ActiveProbeDispatchArgsState = D3D12_RESOURCE_STATE_COMMON;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB   = nullptr;
        u32 FrameSlot  = 0;
        DDGIConstants CPU{};

        struct FCascade {
            Vec3 GridMin{ 0,0,0 };
            f32  Spacing = 1.0f;
            // A origem inteira em celulas mantem o scroll exato.
            int  OriginCells[3]{ 0, 0, 0 };
            int  Scroll[3]{ 0, 0, 0 };
            // Capturada apos um update executado, nao apenas apos um frame renderizado.
            int  PrevOriginCells[3]{ 0, 0, 0 };
        };
        FCascade Cascades[kMaxCascades]{};
        u32  CascadeCount_    = 1;
        static constexpr f32 kCascadeSpacingRatio = 4.0f;
        u32  DesiredCascades  = 2;
        int  CountX = 0, CountY = 0, CountZ = 0;
        u32  ProbesPerCascade_ = 0;
        u32  NumProbes   = 0; // TOTAL = ProbesPerCascade_ * CascadeCount_
        // Prefixo agendado neste frame; AtlasParams.w continua sendo a capacidade total.
        u32  ScheduledProbeCount_ = 0;
        u32  ScheduledCascadeCount_ = 1;
        u32  LastUpdatedCascadeCount_ = 0;
        u32  CascadeUpdateAge_[kMaxCascades]{};
        u64  UpdateSerial_ = 0;
        u64  LastForcedUpdateSerial_ = 0;
        bool InterleavedUpdates = true;
        bool CoarseDue_ = true;
        bool ScheduledFullForced_ = false;
        bool ProbeCompaction = true;
        bool ProbeCompactionThisUpdate_ = false;
        bool LastUpdateUsedProbeCompaction_ = false;
        static constexpr u32 kProbeWakeInterval = 240;
        static constexpr u32 kProbeWakeUpdates  = 2;
        u64 LastProbeWakeSerial_ = 0;
        u32 LastActiveProbeCount_ = 0;
        u32 LastCompactedProbeCapacity_ = 0;
        bool CompactionReadbackIssued_[kTraceTables]{};
        u32 CompactionReadbackCapacity_[kTraceTables]{};
        u32  AtlasWidth  = 0, AtlasHeight = 0;
        u32  DistAtlasWidth = 0, DistAtlasHeight = 0;
        // TilesPerRow e multiplo de CountX para cada banda conter fileiras Z completas.
        u32  TilesPerRow        = 1;
        u32  TileRowsPerCascade = 1; // total de linhas = este x CascadeCount_

        static constexpr D3D12_RESOURCE_STATES kAtlasRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        f32  Intensity    = 1.0f;
        f32  Hysteresis   = kHysteresisMax;
        bool AdaptiveHysteresis = false;
        bool HysteresisResetPending = false;
        // Momentos de distancia exigem reconstrucao temporal mais longa que a irradiancia.
        static constexpr f32 kDistHysteresis = 0.99f;

        // As janelas de invalidacao retêm no maximo kInvalidateResidual do historico antigo.
        static constexpr f32 kInvalidateResidual   = 0.03f;
        static constexpr f32 kInvalidateHysteresis = 0.90f;
        static constexpr u32 kInvalidateFrames =
            DDGIFramesForResidual(kInvalidateHysteresis, kInvalidateResidual);
        static_assert(kInvalidateFrames >= 4 && kInvalidateFrames <= 128,
                      "kInvalidateHysteresis fora da faixa util para o residual pedido");

        static constexpr f32 kInvalidateDistHysteresis = 0.95f;
        static constexpr u32 kInvalidateDistFrames =
            DDGIFramesForResidual(kInvalidateDistHysteresis, kInvalidateResidual);
        static_assert(kInvalidateDistFrames >= kInvalidateFrames,
                      "a janela do dist atlas nao pode ser mais curta que a da irradiancia: ele "
                      "reconstroi de menos amostras, entao precisa de MAIS tempo, nao menos");

        Vec3 InvalidateMin_{};
        Vec3 InvalidateMax_{};
        u32  InvalidateFramesLeft_ = 0;
        u32  InvalidateDistFramesLeft_ = 0;
        // Edicoes de geometria reclassificam quando a janela unificada fecha.
        bool ReclassifyPending_ = false;
        f32  SkyIntensity = 1.0f;
        FRayEpsilonProfile RayEps;
        FGIHitSampling     GIHit;
        FReGIRShaderParams ReGIRParams{};
        FRadianceCacheShaderParams RadianceCacheParams{};
        Vec4               SkyLutParams{};
        f32  MaxRayDist   = 0.0f;
        bool FoliageShadows = true;
        bool Relocation     = true; 
        f32  DeactivationThreshold = 0.20f; 
        bool AdaptiveRays   = true;
        int  MaxRays        = 64;
        int  MinRays        = 16;
        f32  SurfaceBiasScale = 0.2f;
        f32  SurfaceBiasMax   = 0.40f; // metros; 0 = sem teto
        f32  VolumeFadeProbes = 1.0f;  // celulas; 0 = desativado
        static constexpr u32 kRelocateConvergeFrames = 180;
        static constexpr u32 kReclassifyFrames = 6;
        u32  RelocateFramesLeft = 0;
        // Separado da relocacao global: o scroll reclassifica apenas laminas expostas.
        bool ScrolledSinceLastUpdate() const {
            for (u32 C = 0; C < CascadeCount_; ++C)
                for (int A = 0; A < 3; ++A)
                    if (Cascades[C].OriginCells[A] != Cascades[C].PrevOriginCells[A]) return true;
            return false;
        }
        // Reintegra os atlas depois que uma sonda recem-exposta e relocada.
        bool ScrollFollowUpPending = false;

        bool Initialized = false;
        bool Ready       = false;
    };
}
