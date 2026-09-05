#pragma once

#include "SmileEditor/Viewport/RenderThread.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace Smile { class Renderer; }

namespace SmileEditor {
    class ViewportWidget;

    class StatsBridge final : public QObject {
        Q_OBJECT
        Q_PROPERTY(double fps READ GetFPS NOTIFY Updated)
        Q_PROPERTY(double frameTimeMs READ GetFrameTimeMs NOTIFY Updated)
        Q_PROPERTY(int visibleDrawCount READ GetVisibleDrawCount NOTIFY Updated)
        Q_PROPERTY(int occludedDrawCount READ GetOccludedDrawCount NOTIFY Updated)
        Q_PROPERTY(int totalDrawCount READ GetTotalDrawCount NOTIFY Updated)
        Q_PROPERTY(QString internalResolution READ GetInternalResolution NOTIFY Updated)
        Q_PROPERTY(QString outputResolution READ GetOutputResolution NOTIFY Updated)
        Q_PROPERTY(QString gpuName READ GetGPUName NOTIFY Updated)
        Q_PROPERTY(QString vramText READ GetVRAMText NOTIFY Updated)
        Q_PROPERTY(QString vramUsageText READ GetVRAMUsageText NOTIFY Updated)
        Q_PROPERTY(bool vramOverBudget READ IsVRAMOverBudget NOTIFY Updated)
        Q_PROPERTY(double vramBudgetFrac READ GetVRAMBudgetFrac NOTIFY Updated)
        Q_PROPERTY(QString vramNonLocalText READ GetVRAMNonLocalText NOTIFY Updated)
        Q_PROPERTY(QVariantList vramBreakdown READ GetVRAMBreakdown NOTIFY Updated)
        Q_PROPERTY(QString gpuFrameText READ GetGpuFrameText NOTIFY Updated)
        Q_PROPERTY(double gpuFrameMs READ GetGpuFrameMs NOTIFY Updated)
        Q_PROPERTY(QVariantList gpuTimings READ GetGpuTimings NOTIFY Updated)
        Q_PROPERTY(QVariantList shadowCascades READ GetShadowCascades NOTIFY Updated)

        // Telemetria de CPU (Fase 0 do GPU-DRIVEN-PLAN). Nomes explicitos de proposito:
        // sceneMeshDraws NAO e "draws do frame" — exclui agua, particulas, ceu e helpers do
        // editor. Ver Smile::FCpuFrameStats.
        Q_PROPERTY(QVariantList cpuTimings READ GetCpuTimings NOTIFY Updated)
        Q_PROPERTY(QVariantList submitPasses READ GetSubmitPasses NOTIFY Updated)
        Q_PROPERTY(int sceneMeshDraws READ GetSceneMeshDraws NOTIFY Updated)
        Q_PROPERTY(int materialBinds READ GetMaterialBinds NOTIFY Updated)
        // Draws que CONSOMEM MATERIAL e ainda o amarram por item (Fase 3). Nao e
        // "sceneMeshDraws menos bindless": draw sem material — o z-prepass opaco inteiro — nao
        // e fallback de material. Igual a materialBinds enquanto o caminho novo esta desligado,
        // exceto pelo filtro do cache, que este contador nao aplica.
        Q_PROPERTY(int materialFallbackDraws READ GetMaterialFallbackDraws NOTIFY Updated)
        Q_PROPERTY(QString objectConstantsText READ GetObjectConstantsText NOTIFY Updated)
        Q_PROPERTY(QString srvHeapText READ GetSrvHeapText NOTIFY Updated)
        Q_PROPERTY(double srvHeapFrac READ GetSrvHeapFrac NOTIFY Updated)
        // Pool de residencia GPU. Aposentados presos e capacidade acima dos vivos sao o
        // sintoma de um Collect que parou de rodar -- que nao quebra nada, so cresce.
        Q_PROPERTY(QString gpuSlotsText READ GetGpuSlotsText NOTIFY Updated)
        // Tabela canonica de materiais (Fase 2). O sujo por frame tem de ser zero fora de uma
        // edicao: um caminho que suje a tabela por frame devolveria, disfarcado de upload
        // incremental, o custo por frame que a fase existe para remover.
        Q_PROPERTY(QString materialTableText READ GetMaterialTableText NOTIFY Updated)
        // Rebake de micro-map em runtime: memoria retida para poder rebakear, e a VRAM que os
        // BLAS refeitos acrescentaram. Os dois crescem em silencio se algo rebakear demais.
        Q_PROPERTY(QString ommRebakeText READ GetOmmRebakeText NOTIFY Updated)

    public:
        explicit StatsBridge(QObject* Parent = nullptr);

        void SetViewport(ViewportWidget* Value);

        double       GetFPS() const;
        double       GetFrameTimeMs() const;
        int          GetVisibleDrawCount() const;
        int          GetTotalDrawCount() const;
        int          GetOccludedDrawCount() const;
        QString      GetInternalResolution() const;
        QString      GetOutputResolution() const;
        QString      GetGPUName() const;
        QString      GetVRAMText() const;
        QString      GetVRAMUsageText() const;
        bool         IsVRAMOverBudget() const;
        double       GetVRAMBudgetFrac() const;
        QString      GetVRAMNonLocalText() const;
        QVariantList GetVRAMBreakdown() const;
        QString      GetGpuFrameText() const;
        double       GetGpuFrameMs() const;
        QVariantList GetGpuTimings() const;
        QVariantList GetShadowCascades() const;
        QVariantList GetCpuTimings() const;
        QVariantList GetSubmitPasses() const;
        int          GetSceneMeshDraws() const;
        int          GetMaterialBinds() const;
        int          GetMaterialFallbackDraws() const;
        QString      GetObjectConstantsText() const;
        QString      GetSrvHeapText() const;
        QString      GetGpuSlotsText() const;
        QString      GetMaterialTableText() const;
        QString      GetOmmRebakeText() const;
        double       GetSrvHeapFrac() const;

    signals:
        void Updated();

    private:
        void Refresh();
        void Capture(Smile::Renderer& Renderer);
        QVariantList BuildVRAMBreakdown(Smile::Renderer& Renderer) const;
        QVariantList BuildGpuTimings(Smile::Renderer& Renderer);
        QVariantList BuildShadowCascades(Smile::Renderer& Renderer) const;

        struct FSnapshot {
            double       FPS = 0.0;
            int          VisibleDrawCount = 0;
            int          SceneRenderableCount = 0;
            int          OccludedDrawCount = 0;
            QString      InternalResolution = QStringLiteral("—");
            QString      OutputResolution = QStringLiteral("—");
            QString      GPUName = QStringLiteral("Inicializando GPU…");
            QString      VRAMText = QStringLiteral("—");
            QString      VRAMUsageText = QStringLiteral("—");
            bool         VRAMOverBudget = false;
            double       VRAMBudgetFrac = 0.0;
            QString      VRAMNonLocalText = QStringLiteral("—");
            QVariantList VRAMBreakdown;
            QString      GPUFrameText = QStringLiteral("—");
            double       GPUFrameMs = 0.0;
            QVariantList GpuTimings;
            QVariantList ShadowCascades;
            QVariantList CpuTimings;
            QVariantList SubmitPasses;
            int          SceneMeshDraws = 0;
            int          MaterialBinds  = 0;
            int          MaterialFallbackDraws = 0;
            QString      ObjectConstantsText = QStringLiteral("—");
            QString      SrvHeapText  = QStringLiteral("—");
            QString      GpuSlotsText = QStringLiteral("—");
            QString      MaterialTableText = QStringLiteral("—");
            QString      OmmRebakeText = QStringLiteral("—");
            double       SrvHeapFrac = 0.0;
        } Snapshot;

        QPointer<ViewportWidget> Viewport;
        RendererHandle           Renderer;
        QElapsedTimer            RefreshTimer;
        QStringList              GpuTimingOrder;
    };
}
