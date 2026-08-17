#include "SmileEditor/RenderSettingsController.h"
#include "SmileEditor/ViewportWidget.h"
#include "Smile/Graphics/D3D12Device.h"
#include "Smile/Graphics/GpuProfiler.h"
#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Graphics/Renderer.h"

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
        return Out;
    }

    std::optional<FProfileConfiguration> RenderSettingsController::ApplyProfilePreset(
        EProfilePreset _Preset, QString& _Error) {
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

            auto& Tod = Access->GetTimeOfDay();
            Tod.Enabled   = true;
            Tod.Running   = false;
            Tod.TimeHours = 10.0f;

            Result.TimeOfDayHours = Tod.TimeHours;
            // Readback no mesmo lock: a resposta descreve exatamente o estado deixado pelo
            // preset, inclusive degradacoes por capacidade.
            Result.Settings = CollectSettings(*Access);
        }

        // SetDenoiser/SetUpscaler podem reconstruir a lista de alvos. A lista do viewport e
        // cacheada e deve ser relida depois que o lock foi solto.
        if (Viewport) Viewport->NotifyDebugTargetsChanged();
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

        const auto VM = Access->GetDevice().QueryVideoMemory();
        FProfileSnapshot Result;
        Result.FrameIndex   = Access->GetFrameIndex();
        Result.CpuFps       = Viewport->GetFPS();
        Result.OutputWidth  = static_cast<int>(Access->OutputWidth());
        Result.OutputHeight = static_cast<int>(Access->OutputHeight());
        Result.RenderWidth  = static_cast<int>(Access->RenderWidth());
        Result.RenderHeight = static_cast<int>(Access->RenderHeight());
        Result.Gpu          = QString::fromStdWString(Access->GetDevice().GetAdapterDescription());
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
}
