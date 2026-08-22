#include "SmileEditor/Rendering/RenderSettingsController.h"
#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/Debug/GpuProfiler.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Graphics/Renderer/Renderer.h"

#include <cmath>

namespace SmileEditor {
    namespace {
        QVector<FProfileTimingSnapshot> CopyTimings(
            const std::vector<Smile::FGpuProfiler::FScopeResult>& _Results,
            const QString& _Queue) {
            QVector<FProfileTimingSnapshot> Out;
            Out.reserve(static_cast<qsizetype>(_Results.size()));
            for (const auto& R : _Results) {
                Out.append(FProfileTimingSnapshot{
                    QString::fromUtf8(R.Name ? R.Name : ""),
                    _Queue,
                    static_cast<int>(R.Depth),
                    R.Milliseconds,
                    R.RawMilliseconds,
                });
            }
            return Out;
        }
    }

    RenderSettingsController::RenderSettingsController(QObject* _Parent) : QObject(_Parent) {}

    void RenderSettingsController::SetViewport(ViewportWidget* _Value) {
        Viewport = _Value;
        Renderer = Viewport ? Viewport->GetRenderer() : RendererHandle{};
    }

    bool RenderSettingsController::Ready() const {
        if (!Renderer) return false;
        auto Access = Renderer.Lock();
        return Access && Access->IsInitialized();
    }

    FRenderSettingsSnapshot RenderSettingsController::CollectSettings(
        const Smile::Renderer& _Renderer) {
        const auto& Settings = _Renderer.Settings();
        const auto& Tod = _Renderer.GetTimeOfDay();
        FRenderSettingsSnapshot Out;
        Out.DDGI                      = Settings.GetUseGI();
        Out.ReSTIRGI                  = Settings.GetUseReSTIRGI();
        Out.ReSTIRDI                  = Settings.GetUseReSTIRDI();
        Out.RadianceCache             = Settings.GetRadianceCacheEnabled();
        Out.CacheQuery                = Settings.GetRadianceCacheQuery();
        Out.CacheStats                = Settings.GetRadianceCacheStatsEnabled();
        Out.CacheStatsDetail          = Settings.GetRadianceCacheStatsDetailEnabled();
        Out.CacheStatsSource          = Settings.GetRadianceCacheStatsSourceEnabled();
        Out.Reflections               = Settings.GetUseReflections();
        Out.GTAO                      = Settings.GetUseAO();
        Out.IndirectPrimaryRequested  = static_cast<int>(Settings.GetIndirectPrimary());
        Out.IndirectPrimaryEffective  = static_cast<int>(Settings.EffectiveIndirectPrimary());
        Out.IndirectFallbackRequested = static_cast<int>(Settings.GetIndirectFallback());
        Out.IndirectFallbackEffective = static_cast<int>(Settings.EffectiveIndirectFallback());
        Out.Denoiser                  = static_cast<int>(Settings.GetDenoiser());
        Out.Upscaler                  = static_cast<int>(Settings.GetUpscaler());
        Out.UpscalerQuality           = Settings.GetUpscalerQuality();
        Out.RenderScale               = Settings.GetRenderScale();
        Out.TimeOfDayHours            = Tod.TimeHours;
        Out.TimeOfDayRunning          = Tod.Running;
        Out.DIAnalyticCandidates      = static_cast<int>(Settings.GetDIInitialCandidates());
        Out.DIMeshCandidates          = static_cast<int>(Settings.GetDIMeshCandidates());
        Out.DIMeshLightsInPool        = Settings.GetDIMeshLightsInPool();
        Out.DIInitialVisibility       = Settings.GetDIInitialVisibility();
        Out.DIMeshCompactSupport      = Settings.GetDIMeshCompactSupport();
        return Out;
    }

    FCameraPoseSnapshot RenderSettingsController::CollectCamera(
        const Smile::Renderer& _Renderer) {
        const auto Position = _Renderer.GetCameraPos();
        return FCameraPoseSnapshot{
            Position.X,
            Position.Y,
            Position.Z,
            _Renderer.GetPitch(),
            _Renderer.GetYaw(),
        };
    }

    FGIStatusSnapshot RenderSettingsController::CollectGIStatus(
        const Smile::Renderer& _Renderer) {
        const auto& Settings = _Renderer.Settings();
        const auto& DDGI = _Renderer.GetDDGI();
        const auto GridCount = DDGI.GridCount();

        FGIStatusSnapshot Out;
        Out.FrameIndex             = _Renderer.GetFrameIndex();
        Out.Settings               = CollectSettings(_Renderer);
        Out.DDGIInitialized        = DDGI.IsInitialized();
        Out.DesiredCascadeCount    = static_cast<int>(Settings.GetGICascadeCount());
        Out.ActualCascadeCount     = static_cast<int>(DDGI.CascadeCount());
        Out.GridCountX             = static_cast<int>(GridCount.X);
        Out.GridCountY             = static_cast<int>(GridCount.Y);
        Out.GridCountZ             = static_cast<int>(GridCount.Z);
        Out.ProbesPerCascade       = static_cast<int>(DDGI.ProbesPerCascade());
        Out.TotalProbes            = static_cast<int>(DDGI.NumProbesCount());
        Out.RaysPerProbe           = static_cast<int>(DDGI.RaysPerProbe());
        Out.AdaptiveMinRays        = DDGI.GetMinRays();
        Out.AdaptiveMaxRays        = DDGI.GetMaxRays();
        Out.InterleavedUpdates     = DDGI.GetInterleavedUpdates();
        Out.ProbeCompaction        = DDGI.GetProbeCompaction();
        Out.ProbeCompactionEffective = DDGI.LastUpdateUsedProbeCompaction();
        Out.ActiveProbeCount       = static_cast<int>(DDGI.LastActiveProbeCount());
        Out.CompactedProbeCapacity = static_cast<int>(DDGI.LastCompactedProbeCapacity());
        Out.ProbeWakeInterval      = static_cast<int>(DDGI.ProbeWakeInterval());
        Out.LastProbeWakeSerial    = DDGI.LastProbeWakeSerial();
        Out.ScheduledCascadeCount  = static_cast<int>(DDGI.ScheduledCascadeCount());
        Out.LastUpdatedCascadeCount = static_cast<int>(DDGI.LastUpdatedCascadeCount());
        Out.UpdateSerial           = DDGI.UpdateSerial();
        Out.LastForcedUpdateSerial = DDGI.LastForcedUpdateSerial();
        Out.ScheduledFullForced    = DDGI.ScheduledFullWasForced();
        Out.AdaptiveRays           = DDGI.GetAdaptiveRays();
        Out.AdaptiveHysteresis     = DDGI.GetAdaptiveHysteresis();
        Out.Cascades.reserve(Out.ActualCascadeCount);
        for (int Index = 0; Index < Out.ActualCascadeCount; ++Index) {
            const auto GridMin = DDGI.CascadeGridMin(static_cast<Smile::u32>(Index));
            Out.Cascades.append(FDDGICascadeSnapshot{
                Index,
                GridMin.X,
                GridMin.Y,
                GridMin.Z,
                DDGI.CascadeSpacing(static_cast<Smile::u32>(Index)),
                DDGI.CascadeScroll(static_cast<Smile::u32>(Index), 0),
                DDGI.CascadeScroll(static_cast<Smile::u32>(Index), 1),
                DDGI.CascadeScroll(static_cast<Smile::u32>(Index), 2),
                static_cast<int>(DDGI.CascadeUpdateAge(static_cast<Smile::u32>(Index))),
            });
        }
        return Out;
    }

    std::optional<FProfileConfiguration> RenderSettingsController::ApplyProfilePreset(
        EProfilePreset _Preset, QString& _Error, const FProfileOverrides& _Overrides) {
        _Error.clear();
        if (!Renderer) {
            _Error = QStringLiteral("renderer ainda nao esta pronto");
            return std::nullopt;
        }

        FProfileConfiguration Result;
        {
            auto Access = Renderer.Lock();
            if (!Access || !Access->IsInitialized()) {
                _Error = QStringLiteral("renderer ainda nao esta inicializado");
                return std::nullopt;
            }

            // Regime fixo para o A/B: cache ativo de imediato, sol congelado e todos os
            // consumidores do hit path ligados. A ordem preserva o comportamento anterior do
            // McpBridge; os setters continuam sendo os donos de invalidacao e realocacao.
            auto& Settings = Access->Settings();
            Settings.SetRadianceCacheAutoWarmup(false);
            // Instrumentacao altera custo e, no modo detalhado, o trabalho executado. Um preset
            // de benchmark precisa desligar os tres eixos explicitamente, nao depender do estado
            // deixado pela UI ou por uma rodada anterior.
            Settings.SetRadianceCacheStatsEnabled(false);
            Settings.SetRadianceCacheStatsDetailEnabled(false);
            Settings.SetRadianceCacheStatsSourceEnabled(false);
            Settings.SetUseGI(true);
            Settings.SetRadianceCacheEnabled(true);
            Settings.SetRadianceCacheQuery(true);
            Settings.SetUseReSTIRGI(true);
            Settings.SetUseReSTIRDI(true);
            Settings.SetUseReflections(true);
            Settings.SetUseAO(true);
            Settings.SetIndirectPrimary(Smile::EIndirectPrimary::ReSTIR_SHaRC);

            if (_Preset == EProfilePreset::GameplayRR) {
                Result.Preset = QStringLiteral("gameplay_rr");
                Settings.SetUpscalerQuality(0);
                Settings.SetDenoiser(Smile::EDenoiser::DLSS_RR);
            } else {
                Result.Preset = QStringLiteral("controlled_native");
                Settings.SetDenoiser(Smile::EDenoiser::None);
                Settings.SetUpscaler(Smile::EUpscaler::None);
                Settings.SetRenderScale(1.0f);
            }

            // Sobrescritas da matriz da Fase 0, DEPOIS do preset: o preset e o regime base
            // declarado, o override e o unico eixo em teste. Ausente = preserva o valor corrente,
            // entao um sweep so precisa repetir o campo que esta variando.
            if (_Overrides.DIAnalyticCandidates)
                Settings.SetDIInitialCandidates(
                    static_cast<Smile::u32>(*_Overrides.DIAnalyticCandidates));
            if (_Overrides.DIMeshCandidates)
                Settings.SetDIMeshCandidates(
                    static_cast<Smile::u32>(*_Overrides.DIMeshCandidates));
            if (_Overrides.DIMeshLightsInPool)
                Settings.SetDIMeshLightsInPool(*_Overrides.DIMeshLightsInPool);
            if (_Overrides.DIInitialVisibility)
                Settings.SetDIInitialVisibility(*_Overrides.DIInitialVisibility);
            // Muda o DOMINIO amostrado: o setter forca rebuild da tabela e limpa historico.
            if (_Overrides.DIMeshCompactSupport)
                Settings.SetDIMeshCompactSupport(*_Overrides.DIMeshCompactSupport);

            auto& Tod = Access->GetTimeOfDay();
            Tod.Enabled   = true;
            Tod.Running   = false;
            Tod.TimeHours = static_cast<float>(_Overrides.TimeOfDayHours.value_or(10.0));

            Result.TimeOfDayHours = Tod.TimeHours;
            Result.BackgroundThrottleEnabled =
                _Overrides.BackgroundThrottleEnabled.value_or(false);
            // Readback no mesmo lock: a resposta descreve exatamente o estado deixado pelo
            // preset, inclusive degradacoes por capacidade.
            Result.Settings = CollectSettings(*Access);
        }

        // SetDenoiser/SetUpscaler podem reconstruir a lista de alvos. A lista do viewport e
        // cacheada e deve ser relida depois que o lock foi solto.
        if (Viewport) {
            Viewport->SetBackgroundThrottleEnabled(Result.BackgroundThrottleEnabled);
            Viewport->NotifyRendererResourcesChanged();
        }
        emit GISettingsChanged();
        emit RenderSettingsChanged();
        emit StatsChanged();
        emit TimeOfDayChanged();
        return Result;
    }

    std::optional<FProfileSnapshot> RenderSettingsController::ProfileSnapshot(
        QString& _Error) const {
        _Error.clear();
        if (!Renderer || !Viewport) {
            _Error = QStringLiteral("renderer ainda nao esta pronto");
            return std::nullopt;
        }

        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) {
            _Error = QStringLiteral("renderer ainda nao esta inicializado");
            return std::nullopt;
        }

        const auto VM = Access->GetGpuMemoryInfo();
        FProfileSnapshot Result;
        Result.FrameIndex   = Access->GetFrameIndex();
        Result.CpuFps       = Viewport->GetFPS();
        Result.OutputWidth  = static_cast<int>(Access->OutputWidth());
        Result.OutputHeight = static_cast<int>(Access->OutputHeight());
        Result.RenderWidth  = static_cast<int>(Access->RenderWidth());
        Result.RenderHeight = static_cast<int>(Access->RenderHeight());
        Result.Gpu          = QString::fromStdWString(Access->GetGpuDescription());
        Result.Direct       = CopyTimings(Access->GetGpuProfiler().Results(),
                                          QStringLiteral("direct"));
        Result.AsyncCompute = CopyTimings(Access->GetAsyncComputeTimings(),
                                          QStringLiteral("asyncCompute"));
        Result.Vram.Valid              = VM.Valid;
        Result.Vram.LocalUsageBytes    = VM.LocalUsage;
        Result.Vram.LocalBudgetBytes   = VM.LocalBudget;
        Result.Vram.NonLocalUsageBytes = VM.NonLocalUsage;
        Result.Settings = CollectSettings(*Access);
        return Result;
    }

    std::optional<FCameraPoseSnapshot> RenderSettingsController::CameraSnapshot(
        QString& _Error) const {
        _Error.clear();
        if (!Renderer) {
            _Error = QStringLiteral("renderer ainda nao esta pronto");
            return std::nullopt;
        }
        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) {
            _Error = QStringLiteral("renderer ainda nao esta inicializado");
            return std::nullopt;
        }
        return CollectCamera(*Access);
    }

    std::optional<FCameraPoseSnapshot> RenderSettingsController::SetCameraPose(
        const FCameraPoseSnapshot& _Pose, bool _CameraCut, QString& _Error) {
        _Error.clear();
        const bool Finite = std::isfinite(_Pose.X) && std::isfinite(_Pose.Y) &&
                            std::isfinite(_Pose.Z) && std::isfinite(_Pose.PitchDeg) &&
                            std::isfinite(_Pose.YawDeg);
        if (!Finite || std::abs(_Pose.X) > 1'000'000.0 ||
            std::abs(_Pose.Y) > 1'000'000.0 || std::abs(_Pose.Z) > 1'000'000.0 ||
            _Pose.PitchDeg < -89.9 || _Pose.PitchDeg > 89.9 ||
            std::abs(_Pose.YawDeg) > 36'000.0) {
            _Error = QStringLiteral("pose de camera fora dos limites do protocolo");
            return std::nullopt;
        }
        if (!Renderer) {
            _Error = QStringLiteral("renderer ainda nao esta pronto");
            return std::nullopt;
        }

        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) {
            _Error = QStringLiteral("renderer ainda nao esta inicializado");
            return std::nullopt;
        }
        // SetCameraPose tambem protege esta invariavel, mas checar aqui permite devolver erro ao
        // cliente em vez de aceitar um comando que o renderer ignorou silenciosamente.
        if (Access->CaptureBusy()) {
            _Error = QStringLiteral("camera travada durante captura deterministica");
            return std::nullopt;
        }
        Access->SetCameraPose(
            Smile::Vec3{ static_cast<float>(_Pose.X), static_cast<float>(_Pose.Y),
                         static_cast<float>(_Pose.Z) },
            static_cast<float>(_Pose.PitchDeg), static_cast<float>(_Pose.YawDeg),
            _CameraCut);
        return CollectCamera(*Access);
    }

    std::optional<FGIStatusSnapshot> RenderSettingsController::GIStatus(QString& _Error) const {
        _Error.clear();
        if (!Renderer) {
            _Error = QStringLiteral("renderer ainda nao esta pronto");
            return std::nullopt;
        }
        auto Access = Renderer.Lock();
        if (!Access || !Access->IsInitialized()) {
            _Error = QStringLiteral("renderer ainda nao esta inicializado");
            return std::nullopt;
        }
        return CollectGIStatus(*Access);
    }

    std::optional<FGIStatusSnapshot> RenderSettingsController::ApplyGIOverrides(
        const FGIOverrides& _Overrides, QString& _Error) {
        _Error.clear();
        if (!Renderer) {
            _Error = QStringLiteral("renderer ainda nao esta pronto");
            return std::nullopt;
        }

        FGIStatusSnapshot Result;
        {
            auto Access = Renderer.Lock();
            if (!Access || !Access->IsInitialized()) {
                _Error = QStringLiteral("renderer ainda nao esta inicializado");
                return std::nullopt;
            }

            auto& Settings = Access->Settings();
            if (_Overrides.DDGIEnabled)
                Settings.SetUseGI(*_Overrides.DDGIEnabled);
            if (_Overrides.CascadeCount)
                Settings.SetGICascadeCount(
                    static_cast<Smile::u32>(*_Overrides.CascadeCount));
            if (_Overrides.InterleavedUpdates)
                Settings.SetGIInterleavedUpdates(*_Overrides.InterleavedUpdates);
            if (_Overrides.ProbeCompaction)
                Settings.SetGIProbeCompaction(*_Overrides.ProbeCompaction);
            if (_Overrides.AdaptiveRays)
                Settings.SetGIAdaptiveRays(*_Overrides.AdaptiveRays);
            if (_Overrides.AdaptiveHysteresis)
                Settings.SetGIAdaptiveHysteresis(*_Overrides.AdaptiveHysteresis);
            if (_Overrides.IndirectPrimary)
                Settings.SetIndirectPrimary(*_Overrides.IndirectPrimary);
            if (_Overrides.IndirectFallback)
                Settings.SetIndirectFallback(*_Overrides.IndirectFallback);

            // Readback no mesmo lock. Desired/actual separados denunciam uma realocacao que nao
            // materializou, e requested/effective denunciam degradacao da politica.
            Result = CollectGIStatus(*Access);
        }

        emit GISettingsChanged();
        emit RenderSettingsChanged();
        emit StatsChanged();
        return Result;
    }
}
