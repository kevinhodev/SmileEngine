#pragma once

#include "SmileEditor/RenderThread.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <optional>

namespace Smile { class Renderer; }

namespace SmileEditor {
    class ViewportWidget;

    enum class EProfilePreset {
        GameplayRR,
        ControlledNative,
    };

    struct FProfileTimingSnapshot {
        QString Name;
        QString Queue;
        int     Depth           = 0;
        double  Milliseconds    = 0.0;
        double  RawMilliseconds = 0.0;
    };

    struct FVideoMemorySnapshot {
        bool    Valid              = false;
        quint64 LocalUsageBytes    = 0;
        quint64 LocalBudgetBytes   = 0;
        quint64 NonLocalUsageBytes = 0;
    };

    // Estado relido depois de uma mutacao. Pedido e efetivo ficam juntos para nenhum adaptador
    // publicar "SHaRC" quando o frame degradou para DDGI/Off por capacidade.
    struct FRenderSettingsSnapshot {
        bool   DDGI                  = false;
        bool   ReSTIRGI              = false;
        bool   ReSTIRDI              = false;
        bool   RadianceCache         = false;
        bool   CacheQuery            = false;
        bool   Reflections           = false;
        bool   GTAO                  = false;
        int    IndirectPrimaryRequested  = 0;
        int    IndirectPrimaryEffective  = 0;
        int    IndirectFallbackRequested = 0;
        int    IndirectFallbackEffective = 0;
        int    Denoiser              = 0;
        int    Upscaler              = 0;
        int    UpscalerQuality       = 0;
        double RenderScale           = 1.0;
        double TimeOfDayHours        = 0.0;
        bool   TimeOfDayRunning      = false;
        // Knobs da matriz da Fase 0 do MESH-LIGHTS-PLAN.md. Entram no READBACK, e nao so na
        // aplicacao: quem roda o sweep precisa confirmar o degrau que de fato ficou, sem inferir
        // do que pediu. Mesmo contrato do resto deste struct.
        int    DIAnalyticCandidates  = 0;
        int    DIMeshCandidates      = 0;
        bool   DIMeshLightsInPool    = false;
        bool   DIInitialVisibility   = false;
        bool   DIMeshCompactSupport  = false;
    };

    // Sobrescritas OPCIONAIS do profile_configure. Os knobs de amostragem ausentes preservam o
    // valor corrente — um default neles reescreveria o eixo que nao esta em teste. A hora e a
    // excecao deliberada: ausente preserva o baseline historico e deterministico de 10:00;
    // presente permite que a mesma regua rode de noite sem uma mutacao manual posterior.
    //
    // Nao viraram comando MCP proprio de proposito: o regime deterministico ja e responsabilidade
    // do profile_configure, e um segundo comando abriria a possibilidade de configurar knob sem
    // fixar preset/TOD — exatamente a captura sem regime declarado que o protocolo proibe.
    struct FProfileOverrides {
        std::optional<double> TimeOfDayHours;
        std::optional<int>  DIAnalyticCandidates;
        std::optional<int>  DIMeshCandidates;
        std::optional<bool> DIMeshLightsInPool;
        std::optional<bool> DIInitialVisibility;
        std::optional<bool> DIMeshCompactSupport;
    };

    struct FProfileSnapshot {
        quint64 FrameIndex = 0;
        double  CpuFps     = 0.0;
        int     OutputWidth  = 0;
        int     OutputHeight = 0;
        int     RenderWidth  = 0;
        int     RenderHeight = 0;
        QString Gpu;
        QVector<FProfileTimingSnapshot> Direct;
        QVector<FProfileTimingSnapshot> AsyncCompute;
        FVideoMemorySnapshot            Vram;
        FRenderSettingsSnapshot         Settings;
    };

    struct FProfileConfiguration {
        QString Preset;
        double  TimeOfDayHours = 0.0;
        FRenderSettingsSnapshot Settings;
    };

    // Fachada compartilhada pelos adaptadores do editor. Ela e dona da fronteira com o
    // RendererHandle: McpBridge traduz protocolo, RenderSettingsBridge traduz QML, e nenhum dos
    // dois precisa duplicar a regra de lock/readback/notificacao para configuracoes externas.
    class RenderSettingsController final : public QObject {
        Q_OBJECT

    public:
        explicit RenderSettingsController(QObject* Parent = nullptr);

        void SetViewport(ViewportWidget* Value);
        bool Ready() const;

        // As sobrescritas sao aplicadas DEPOIS do preset, para o preset continuar sendo o regime
        // base declarado e o override ser so o eixo em teste. Invertida, a ordem faria o preset
        // apagar o degrau do sweep silenciosamente.
        std::optional<FProfileConfiguration> ApplyProfilePreset(
            EProfilePreset Preset, QString& Error,
            const FProfileOverrides& Overrides = {});
        std::optional<FProfileSnapshot> ProfileSnapshot(QString& Error) const;

    signals:
        // Emitidos sempre depois de soltar o lock. Os bridges QML os repassam como NOTIFY das
        // propriedades que o controlador alterou por uma porta externa.
        void GISettingsChanged();
        void RenderSettingsChanged();
        void StatsChanged();
        void TimeOfDayChanged();

    private:
        static FRenderSettingsSnapshot CollectSettings(const Smile::Renderer& Renderer);

        RendererHandle  Renderer;
        ViewportWidget* Viewport = nullptr;
    };
}
