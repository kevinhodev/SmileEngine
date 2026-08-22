#include "SmileEditor/Rendering/RenderSettingsBridge.h"
#include "SmileEditor/Viewport/ViewportWidget.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"

#include <QVariantMap>

#include <algorithm>
#include <cmath>

// Corpos movidos do ViewportWidget.cpp, sem reescrita: mesmos clamps, mesmos defaults de
// "antes do renderer existir" e mesmos sinais. Este passo e de ORGANIZACAO — se um valor
// mudar, e erro de digitacao no move.
namespace SmileEditor {
    RenderSettingsBridge::RenderSettingsBridge(QObject* _Parent) : QObject(_Parent) {}

    void RenderSettingsBridge::SetRenderer(RendererHandle _R) {
        Renderer = std::move(_R);
        emit AvailableChanged();
        // Uma emissao por dominio: o QML reavalia os cards com os valores reais do motor, que
        // podem divergir dos defaults compilados na bridge.
        emit SunShaftsSettingsChanged();
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
        emit RenderSettingsChanged();
        emit RendererInitialized();
    }

    // === Sun shafts e fog volumetrico ===================================================

    bool RenderSettingsBridge::AreSunShaftsEnabled() const {
        return Renderer ? Renderer->Settings().GetUseSunShafts() : true;
    }

    void RenderSettingsBridge::SetSunShaftsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseSunShafts(_Enabled);
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsIntensity() const {
        return Renderer ? Renderer->Settings().GetShaftsIntensity() : 1.0;
    }

    void RenderSettingsBridge::SetSunShaftsIntensity(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsIntensity(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsDust() const {
        return Renderer ? Renderer->Settings().GetShaftsDust() : 8.0;
    }

    void RenderSettingsBridge::SetSunShaftsDust(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsDust(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsPhaseG() const {
        return Renderer ? Renderer->Settings().GetShaftsPhaseG() : 0.7;
    }

    void RenderSettingsBridge::SetSunShaftsPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsPhaseG(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    int RenderSettingsBridge::GetSunShaftsSteps() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetShaftsSteps()) : 32;
    }

    void RenderSettingsBridge::SetSunShaftsSteps(int _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsSteps(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    double RenderSettingsBridge::GetSunShaftsRange() const {
        return Renderer ? Renderer->Settings().GetShaftsMaxDist() : 128.0;
    }

    void RenderSettingsBridge::SetSunShaftsRange(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsMaxDist(static_cast<Smile::f32>(_Value));
        emit SunShaftsSettingsChanged();
    }

    bool RenderSettingsBridge::AreSunShaftsTemporal() const {
        return Renderer ? Renderer->Settings().GetShaftsTemporal() : true;
    }

    void RenderSettingsBridge::SetSunShaftsTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetShaftsTemporal(_Enabled);
        emit SunShaftsSettingsChanged();
    }

    bool RenderSettingsBridge::IsVolFogEnabled() const {
        return Renderer ? Renderer->Settings().GetUseVolumetricFog() : true;
    }

    void RenderSettingsBridge::SetVolFogEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseVolumetricFog(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogDistance() const {
        return Renderer ? Renderer->Settings().GetVolFogMaxDistance() : 100.0;
    }

    void RenderSettingsBridge::SetVolFogDistance(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogMaxDistance(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogPhaseG() const {
        return Renderer ? Renderer->Settings().GetVolFogPhaseG() : 0.3;
    }

    void RenderSettingsBridge::SetVolFogPhaseG(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogPhaseG(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogDensity() const {
        return Renderer ? Renderer->Settings().GetVolFogExtinctionScale() : 1.0;
    }

    void RenderSettingsBridge::SetVolFogDensity(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogExtinctionScale(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogAmbient() const {
        return Renderer ? Renderer->Settings().GetVolFogAmbientIntensity() : 1.0;
    }

    void RenderSettingsBridge::SetVolFogAmbient(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogAmbientIntensity(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetHeightFogSkyContribution() const {
        return Renderer ? Renderer->Settings().GetFogHeightSkyContribution() : 1.0;
    }

    void RenderSettingsBridge::SetHeightFogSkyContribution(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetFogHeightSkyContribution(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsPerPixelAtmoTransmittance() const {
        return Renderer ? Renderer->Settings().GetPerPixelAtmoTransmittance() : true;
    }

    void RenderSettingsBridge::SetPerPixelAtmoTransmittance(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetPerPixelAtmoTransmittance(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsSkyAmbientSH() const {
        return Renderer ? Renderer->Settings().GetSkyAmbientSH() : true;
    }

    void RenderSettingsBridge::SetSkyAmbientSH(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetSkyAmbientSH(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsVolFogTemporal() const {
        return Renderer ? Renderer->Settings().GetVolFogTemporal() : true;
    }

    void RenderSettingsBridge::SetVolFogTemporal(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogTemporal(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetVolFogLights() const {
        return Renderer ? Renderer->Settings().GetVolFogLightsIntensity() : 1.0;
    }

    void RenderSettingsBridge::SetVolFogLights(double _Value) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogLightsIntensity(static_cast<Smile::f32>(_Value));
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    bool RenderSettingsBridge::IsVolFogConsDepth() const {
        return Renderer ? Renderer->Settings().GetVolFogConservativeDepth() : true;
    }

    void RenderSettingsBridge::SetVolFogConsDepth(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetVolFogConservativeDepth(_Enabled);
        emit VolFogSettingsChanged();
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    // === Sombras do sol =================================================================

    bool RenderSettingsBridge::AreSunShadowsEnabled() const {
        return Renderer && Renderer->Settings().GetUseSunShadows();
    }

    bool RenderSettingsBridge::IsShadowCacheEnabled() const {
        return Renderer && Renderer->Settings().GetShadowCascadeCache();
    }

    bool RenderSettingsBridge::IsShadowDebugCascades() const {
        return Renderer && Renderer->Settings().GetShadowDebugCascades();
    }

    double RenderSettingsBridge::GetShadowMaxDistance() const {
        return Renderer ? Renderer->Settings().GetShadowMaxDistance() : 800.0;
    }

    double RenderSettingsBridge::GetShadowDepthBias() const {
        return Renderer ? Renderer->Settings().GetShadowDepthBias() : 2.0; // texels da cascata
    }

    double RenderSettingsBridge::GetShadowNormalOffset() const {
        return Renderer ? Renderer->Settings().GetShadowNormalOffset() : 2.5;
    }


    double RenderSettingsBridge::GetShadowMinCasterTexels() const {
        return Renderer ? Renderer->Settings().GetShadowMinCasterTexels() : 2.0;
    }

    QVariantList RenderSettingsBridge::GetShadowCascadeBias() const {
        QVariantList List;
        for (Smile::u32 c = 0; c < Smile::FSunShadows::kNumCascades; ++c)
            List.append(Renderer ? Renderer->Settings().GetShadowCascadeBiasScale(c) : 1.0);
        return List;
    }

    void RenderSettingsBridge::SetSunShadowsEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseSunShadows(_Enabled);
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowCacheEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowCascadeCache(_Enabled);
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowDebugCascades(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowDebugCascades(_Enabled);
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowMaxDistance(double _Distance) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowMaxDistance(static_cast<Smile::f32>(_Distance));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowDepthBias(double _Bias) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowDepthBias(static_cast<Smile::f32>(_Bias));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowNormalOffset(double _Texels) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowNormalOffset(static_cast<Smile::f32>(_Texels));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowMinCasterTexels(double _Texels) {
        if (!Renderer) return;
        Renderer->Settings().SetShadowMinCasterTexels(static_cast<Smile::f32>(_Texels));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetShadowCascadeBiasScale(int _Cascade, double _Scale) {
        if (!Renderer || _Cascade < 0) return;
        Renderer->Settings().SetShadowCascadeBiasScale(static_cast<Smile::u32>(_Cascade),
                                                      static_cast<Smile::f32>(_Scale));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    double RenderSettingsBridge::GetShadowSunAngle() const {
        return Renderer ? Renderer->Settings().GetSunAngularSize() : 0.53;
    }

    void RenderSettingsBridge::SetShadowSunAngle(double _Degrees) {
        if (!Renderer) return;
        Renderer->Settings().SetSunAngularSize(static_cast<Smile::f32>(_Degrees));
        emit ShadowSettingsChanged();
        emit GISettingsChanged();
    }

    // === Iluminacao global ==============================================================

    bool RenderSettingsBridge::IsDDGIEnabled() const {
        return Renderer && Renderer->Settings().GetUseGI();
    }

    bool RenderSettingsBridge::IsReSTIRGIEnabled() const {
        return Renderer && Renderer->Settings().GetUseReSTIRGI();
    }

    bool RenderSettingsBridge::IsReSTIRGIHalfRes() const {
        return Renderer && Renderer->Settings().GetGIHalfRes();
    }

    bool RenderSettingsBridge::IsReGIREnabled() const {
        return Renderer && Renderer->Settings().GetUseReGIR();
    }

    bool RenderSettingsBridge::IsReSTIRGIVisibilityEnabled() const {
        return Renderer && Renderer->Settings().GetGIVisibility();
    }

    bool RenderSettingsBridge::AreGIFoliageShadowsEnabled() const {
        return Renderer && Renderer->Settings().GetGIFoliageShadows();
    }

    bool RenderSettingsBridge::IsGIAdaptiveHysteresisEnabled() const {
        return Renderer && Renderer->Settings().GetGIAdaptiveHysteresis();
    }

    bool RenderSettingsBridge::IsGIAdaptiveRaysEnabled() const {
        return Renderer && Renderer->Settings().GetGIAdaptiveRays();
    }

    int RenderSettingsBridge::GetGICascadeCount() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetGICascadeCount()) : 1;
    }

    bool RenderSettingsBridge::IsGIMeasureTerminatorOff() const {
        return Renderer && Renderer->Settings().GetGIMeasureTerminatorOff();
    }

    bool RenderSettingsBridge::IsReflectionsCullBackfaceEnabled() const {
        return Renderer && Renderer->Settings().GetReflectionsCullBackface();
    }

    bool RenderSettingsBridge::IsReSTIRDIEnabled() const {
        return Renderer && Renderer->Settings().GetUseReSTIRDI();
    }

    bool RenderSettingsBridge::IsGIBackfacePolicyEnabled() const {
        return Renderer && Renderer->Settings().GetGIBackfacePolicy();
    }

    double RenderSettingsBridge::GetGISurfaceBiasMax() const {
        return Renderer ? static_cast<double>(Renderer->Settings().GetGISurfaceBiasMax()) : 0.0;
    }

    double RenderSettingsBridge::GetGIVolumeFadeProbes() const {
        return Renderer ? static_cast<double>(Renderer->Settings().GetGIVolumeFadeProbes()) : 0.0;
    }


    void RenderSettingsBridge::ToggleDDGI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseGI(!RendererAccess->Settings().GetUseGI());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleReSTIRGI() {
        if (!Renderer) return;
        bool SourceDebugDropped = false;
        {
            auto RendererAccess = Renderer.Lock();
            auto& Settings = RendererAccess->Settings();
            const bool MapBefore = Settings.GetGISourceDebug();
            Settings.SetUseReSTIRGI(!Settings.GetUseReSTIRGI());
            // Desligar este passe DERRUBA o mapa da fonte (ele é escrito no RecordTrace daqui), e
            // isso tira um alvo do registro — a janela precisa saber, exatamente como quando o
            // operador mexe no toggle do mapa.
            SourceDebugDropped = MapBefore && !Settings.GetGISourceDebug();
        } // lock solto antes dos sinais (o QML relê no ato e pega o mesmo lock)

        emit GISettingsChanged();
        if (SourceDebugDropped && Viewport) Viewport->NotifyRendererResourcesChanged();
    }

    void RenderSettingsBridge::ToggleReSTIRGIHalfRes() {
        if (!Renderer) return;
        {
            auto RendererAccess = Renderer.Lock();
            auto& Settings = RendererAccess->Settings();
            Settings.SetGIHalfRes(!Settings.GetGIHalfRes());
        }
        emit GISettingsChanged();
        // Atualiza consumidores dos recursos realocados.
        if (Viewport) Viewport->NotifyRendererResourcesChanged();
    }

    void RenderSettingsBridge::ToggleReGIR() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseReGIR(!RendererAccess->Settings().GetUseReGIR());
        emit GISettingsChanged();
    }

    // --- World radiance cache -----------------------------------------------------------
    bool RenderSettingsBridge::IsRadianceCacheEnabled() const {
        return Renderer && Renderer->Settings().GetRadianceCacheEnabled();
    }
    bool RenderSettingsBridge::IsRadianceCacheQuery() const {
        return Renderer && Renderer->Settings().GetRadianceCacheQuery();
    }
    bool RenderSettingsBridge::IsRadianceCacheAutoWarmup() const {
        return Renderer && Renderer->Settings().GetRadianceCacheAutoWarmup();
    }
    QString RenderSettingsBridge::GetRadianceCacheWarmup() const {
        if (!Renderer) return QStringLiteral("—");
        const auto& S = Renderer->Settings();
        const QString Name = QString::fromLatin1(S.RadianceCacheWarmupName());
        // Os frames so aparecem em `filling`: fora dele o contador vale zero e imprimi-lo daria a
        // impressao de um relogio parado.
        if (Name != QStringLiteral("filling")) return Name;
        return Name + QStringLiteral(" (%1 frames)").arg(S.RadianceCacheWarmupFrames());
    }
    bool RenderSettingsBridge::IsRadianceCacheStats() const {
        return Renderer && Renderer->Settings().GetRadianceCacheStatsEnabled();
    }
    bool RenderSettingsBridge::IsRadianceCacheStatsDetail() const {
        return Renderer && Renderer->Settings().GetRadianceCacheStatsDetailEnabled();
    }
    bool RenderSettingsBridge::IsRadianceCacheStatsSource() const {
        return Renderer && Renderer->Settings().GetRadianceCacheStatsSourceEnabled();
    }
    QString RenderSettingsBridge::GetRadianceCacheSourceBreakdown() const {
        if (!Renderer) return QString();
        // Só `Source`, e NÃO a instrumentação-base junto: `Sf` de reflexões-only é regime
        // suportado e nele `Meta.Stats` é falso (o bit de consulta exige query aberta). Exigir a
        // base aqui esconderia exatamente o caso que o contador de fonte veio medir — os
        // contadores de fonte têm denominador PRÓPRIO e não dependem dos de consulta.
        Smile::FRadianceCacheSnapshot Snap;
        if (!CacheSnapshot(Snap) || !Snap.Meta.Source) return QString();
        const auto& S = Snap.Stats;
        // ZERO MEDIDO não é "não medido". Um frame sem nenhum hit secundário — câmera para o céu,
        // ou todos os raios escapando — é medição válida, e imprimir "off" ali esconderia
        // justamente o caso que faria alguém desconfiar do número.
        if (S.SrcTotal == 0) return QStringLiteral("fonte: nenhum hit sombreado neste frame");
        const double T = static_cast<double>(S.SrcTotal);
        auto Pct = [T](Smile::u32 N) {
            return QString::number(100.0 * static_cast<double>(N) / T, 'f', 1) +
                   QStringLiteral("%");
        };
        // A SOMA é impressa junto, e não como conferência do desenvolvedor: as TRÊS fontes são
        // mutuamente exclusivas por construção, então qualquer sobra é defeito do instrumento —
        // e um instrumento que só se valida em teste offline não se valida quando importa.
        const Smile::u32 Sum = S.SrcCache + S.SrcDDGI + S.SrcZero;
        const QString Check = (Sum == S.SrcTotal)
            ? QStringLiteral("✓")
            : QStringLiteral("✗ sobra %1").arg(static_cast<long long>(S.SrcTotal) -
                                               static_cast<long long>(Sum));
        // `inelegível` vem depois do travessão porque é OUTRO EIXO. Ele cruza com DDGI e com zero
        // — nunca com cache, por construção —, e o texto diz exatamente isso e nada além: os
        // totais marginais NÃO respondem quanto da inelegibilidade caiu especificamente no DDGI.
        // Afirmar "é a parcela estrutural do DDGI" seria inventar uma interseção não medida; para
        // tê-la seria preciso um contador de `ineligible && ddgiAnswered`, que não existe.
        return QStringLiteral("fonte: cache %1 · DDGI %2 · zero %3 (%4 hits · soma %5)"
                              " — inelegíveis %6 (em DDGI ou zero)")
            .arg(Pct(S.SrcCache), Pct(S.SrcDDGI), Pct(S.SrcZero))
            .arg(S.SrcTotal).arg(Check).arg(Pct(S.SrcIneligible));
    }
    bool RenderSettingsBridge::IsGISourceDebug() const {
        return Renderer && Renderer->Settings().GetGISourceDebug();
    }
    bool RenderSettingsBridge::IsRadianceCacheDedicatedUpdate() const {
        return Renderer && Renderer->Settings().GetRadianceCacheDedicatedUpdate();
    }
    bool RenderSettingsBridge::IsRadianceCacheCompactUpdate() const {
        return Renderer && Renderer->Settings().GetRadianceCacheCompactUpdate();
    }
    double RenderSettingsBridge::GetRadianceCacheUpdateFraction() const {
        return Renderer ? Renderer->Settings().GetRadianceCacheUpdateFraction() : 0.04;
    }
    bool RenderSettingsBridge::IsRadianceCachePrevTerminal() const {
        return Renderer && Renderer->Settings().GetRadianceCacheUsePrevTerminal();
    }
    int RenderSettingsBridge::GetRadianceCacheMaxVertices() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetRadianceCacheMaxVertices()) : 4;
    }
    double RenderSettingsBridge::GetRadianceCacheMinRoughness() const {
        return Renderer ? Renderer->Settings().GetRadianceCacheMinCacheableRoughness() : 0.5;
    }
    int RenderSettingsBridge::GetRadianceCacheMinSamples() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetRadianceCacheMinSampleCount())
                        : 4;
    }
    double RenderSettingsBridge::GetRadianceCacheCellSize() const {
        return Renderer ? Renderer->Settings().GetRadianceCacheCellSize() : 0.50;
    }
    double RenderSettingsBridge::GetRadianceCacheLodDistance() const {
        return Renderer ? Renderer->Settings().GetRadianceCacheLodDistance() : 6.0;
    }
    int RenderSettingsBridge::GetRadianceCacheDebugMode() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetRadianceCacheDebugMode()) : 0;
    }
    // UMA leitura, por VALOR, sob UM lock. O `Renderer->` devolve um Access temporário que solta o
    // lock no fim da expressão (ver RenderThread.h), então guardar referência para os contadores e
    // pedir o meta numa segunda chamada podia atravessar um frame: meta novo, contador velho — o
    // par exato que o meta veio impedir. Todos os getters de telemetria passam por aqui.
    bool RenderSettingsBridge::CacheSnapshot(Smile::FRadianceCacheSnapshot& _Out) const {
        if (!Renderer) return false;
        auto A = Renderer.Lock();
        if (!A) return false;
        _Out = A->Settings().RadianceCacheSnapshot();
        return _Out.Meta.Valid;
    }

    double RenderSettingsBridge::GetRadianceCacheOccupancy() const {
        Smile::FRadianceCacheSnapshot Snap;
        if (!CacheSnapshot(Snap) || Snap.Capacity == 0) return 0.0;
        return 100.0 * static_cast<double>(Snap.Stats.Occupied) /
               static_cast<double>(Snap.Capacity);
    }
    double RenderSettingsBridge::GetRadianceCacheHitRate() const {
        Smile::FRadianceCacheSnapshot Snap;
        // Exige o regime DO SNAPSHOT: sem `Stats` os contadores de consulta nem rodaram, e o
        // quociente seria 0/0 apresentado como 0%.
        if (!CacheSnapshot(Snap) || !Snap.Meta.Stats || Snap.Stats.Queries == 0) return 0.0;
        return 100.0 * static_cast<double>(Snap.Stats.Hits) /
               static_cast<double>(Snap.Stats.Queries);
    }
    double RenderSettingsBridge::GetRadianceCacheConvergence() const {
        Smile::FRadianceCacheSnapshot Snap;
        if (!CacheSnapshot(Snap) || Snap.Stats.HasSamples == 0) return 0.0;
        // Denominador = celulas COM AMOSTRA, e nao as confiaveis: isto e "quantas amostras tem, em
        // media, a celula que tem alguma". Dividir pelas confiaveis excluiria justamente as que
        // estao aquecendo, e a media subiria por construcao quando o piso subisse.
        return static_cast<double>(Snap.Stats.Samples) /
               static_cast<double>(Snap.Stats.HasSamples);
    }
    double RenderSettingsBridge::GetRadianceCacheMemoryMB() const {
        // A memória é do RECURSO, não do snapshot: ela vale enquanto o cache existir, e some
        // sozinha quando a capacidade zera. Não passa pelo `Valid`.
        if (!Renderer) return 0.0;
        return static_cast<double>(Renderer->Settings().RadianceCacheBytes()) / (1024.0 * 1024.0);
    }
    QString RenderSettingsBridge::GetIndirectPolicySummary() const {
        if (!Renderer) return QString();
        auto A = Renderer.Lock();          // um lock só: os quatro descrevem o mesmo instante
        if (!A) return QString();
        const auto& S = A->Settings();
        const QString PReq = QString::fromLatin1(Smile::IndirectPrimaryName(S.GetIndirectPrimary()));
        const QString PEff = QString::fromLatin1(
            Smile::IndirectPrimaryName(S.EffectiveIndirectPrimary()));
        const QString FReq = QString::fromLatin1(
            Smile::IndirectFallbackName(S.GetIndirectFallback()));
        const QString FEff = QString::fromLatin1(
            Smile::IndirectFallbackName(S.EffectiveIndirectFallback()));
        // A seta só aparece quando há divergência: iguais, ela seria ruído; diferentes, ela é a
        // única coisa que interessa na linha.
        auto Pair = [](const QString& Req, const QString& Eff) {
            return Req == Eff ? Req : QStringLiteral("%1 → %2").arg(Req, Eff);
        };
        return QStringLiteral("primário: %1  ·  fallback: %2")
            .arg(Pair(PReq, PEff), Pair(FReq, FEff));
    }
    int RenderSettingsBridge::GetIndirectPrimary() const {
        if (!Renderer) return 0;
        return static_cast<int>(Renderer->Settings().GetIndirectPrimary());
    }
    int RenderSettingsBridge::GetIndirectFallback() const {
        if (!Renderer) return 0;
        return static_cast<int>(Renderer->Settings().GetIndirectFallback());
    }
    void RenderSettingsBridge::SetIndirectPrimary(int _V) {
        if (!Renderer) return;
        // Faixa checada aqui, e nao com um default no switch: um indice fora da faixa e bug de QML,
        // e cair em ReSTIR_SHaRC em silencio trocaria o estimador da cena por causa de um typo.
        if (_V < 0 || _V > static_cast<int>(Smile::EIndirectPrimary::Off)) return;
        bool SourceDebugDropped = false;
        {
            auto RendererAccess = Renderer.Lock();
            auto& Settings = RendererAccess->Settings();
            const auto V = static_cast<Smile::EIndirectPrimary>(_V);
            if (V == Settings.GetIndirectPrimary()) return;
            const bool MapBefore = Settings.GetGISourceDebug();
            Settings.SetIndirectPrimary(V);
            // Mesmo caso do ToggleReSTIRGI: sair de SHaRC para o produtor do mapa da fonte, que e
            // o RecordTrace do ReSTIR GI. O detector de borda do renderer faria isso sozinho no
            // proximo frame, mas ele nao tem como notificar a janela de debug — entao o caminho do
            // OPERADOR resolve aqui, sincrono, e o detector segue cobrindo os automaticos.
            //
            // O historico NAO e derrubado aqui: quem derruba e o detector, que ve a borda do valor
            // EFETIVO. Invalidar tambem no setter cancelaria a captura duas vezes pelo mesmo evento.
            Settings.DropGISourceDebugIfOrphaned();
            SourceDebugDropped = MapBefore && !Settings.GetGISourceDebug();
        } // lock solto antes dos sinais

        emit GISettingsChanged();
        if (SourceDebugDropped && Viewport) Viewport->NotifyRendererResourcesChanged();
    }
    void RenderSettingsBridge::SetIndirectFallback(int _V) {
        if (!Renderer) return;
        if (_V < 0 || _V > static_cast<int>(Smile::EIndirectFallback::Black)) return;
        {
            auto RendererAccess = Renderer.Lock();
            auto& Settings = RendererAccess->Settings();
            const auto V = static_cast<Smile::EIndirectFallback>(_V);
            if (V == Settings.GetIndirectFallback()) return;
            // Nao mexe no mapa da fonte: o produtor dele e o primario, e o fallback so troca o que
            // o raio devolve no miss. O mapa continua tendo o que pintar — e a cor muda, que e
            // exatamente o que se quer ver.
            Settings.SetIndirectFallback(V);
        }
        emit GISettingsChanged();
    }
    QString RenderSettingsBridge::GetRadianceCacheSummary() const {
        if (!Renderer) return QString();
        Smile::FRadianceCacheSnapshot Snap;
        const bool Live = CacheSnapshot(Snap);
        if (Snap.Capacity == 0) return QStringLiteral("cache não montado");
        // SEM snapshot válido não há o que resumir. Antes esta linha imprimia os últimos números
        // conhecidos ao lado de toggles atuais — com o cache desligado, para sempre.
        if (!Live) return QStringLiteral("%1 células de capacidade · sem medição no frame")
                              .arg(Snap.Capacity);
        // A OCUPAÇÃO entra no texto daqui, e não é concatenada no QML a partir da propriedade
        // numérica: sem medição aquela propriedade devolve 0,0, e "sem medição · 0,0%" lê como
        // ocupação zero — que é um estado real e completamente diferente. Número que só significa
        // algo junto de uma condição não pode ser publicado sozinho.
        const double Occ = Snap.Capacity > 0
            ? 100.0 * static_cast<double>(Snap.Stats.Occupied) /
                  static_cast<double>(Snap.Capacity)
            : 0.0;
        // Composto de UMA cópia, e não chamando os getters acima: cada um deles pega o próprio
        // lock, e a linha misturaria números de frames diferentes.
        const QString Hit = Snap.Meta.Stats
            ? QString::number(Snap.Stats.Queries > 0
                                  ? 100.0 * static_cast<double>(Snap.Stats.Hits) /
                                        static_cast<double>(Snap.Stats.Queries)
                                  : 0.0, 'f', 1) + QStringLiteral("%")
            : QStringLiteral("— (instrumentação off)");
        const double Conv = Snap.Stats.HasSamples > 0
            ? static_cast<double>(Snap.Stats.Samples) /
                  static_cast<double>(Snap.Stats.HasSamples)
            : 0.0;
        // Confiáveis ao lado de ocupadas: são o número que diz quanto do cache de fato encerra um
        // caminho, e a distância entre os dois é o aquecimento em curso.
        return QStringLiteral("%1 / %2 células (%3%) · %4 confiáveis · acerto %5 · "
                              "%6 amostras/célula · despejadas %7")
            .arg(Snap.Stats.Occupied).arg(Snap.Capacity)
            .arg(QString::number(Occ, 'f', 1).replace('.', ','))
            .arg(Snap.Stats.Confident).arg(Hit)
            .arg(QString::number(Conv, 'f', 1))
            .arg(Snap.Stats.Evicted);
    }

    QString RenderSettingsBridge::GetRadianceCacheMissBreakdown() const {
        // String VAZIA quando não há o que mostrar, e não um aviso: a linha some, e é o próprio
        // QML que decide isso perguntando `text.length`. Um "detalhe desligado" na tela ocupava
        // altura para repetir o que o toggle logo acima já diz.
        //
        // Os DOIS sub-regimes que esta linha precisa saem do snapshot: `Detail` produz os motivos,
        // `Stats` produz o denominador (as consultas). Ligar o detalhe agora não faz os números de
        // dois frames atrás terem sido medidos com ele.
        Smile::FRadianceCacheSnapshot Snap;
        if (!CacheSnapshot(Snap) || !Snap.Meta.Detail || !Snap.Meta.Stats) return QString();
        const auto& S = Snap.Stats;
        // Percentual sobre as CONSULTAS, e nao sobre os misses: "12% dos raios pararam por cone
        // estreito" e uma frase acionavel; "38% dos erros" depende de quantos erros houve e muda
        // de significado sozinha quando o cache aquece.
        const double Q = S.Queries > 0 ? static_cast<double>(S.Queries) : 0.0;
        auto Pct = [Q](Smile::u32 N) {
            if (Q <= 0.0) return QStringLiteral("—");
            return QString::number(100.0 * static_cast<double>(N) / Q, 'f', 1) +
                   QStringLiteral("%");
        };
        // A insercao tem denominador PROPRIO (tentativas de escrita), e nao as consultas: sao
        // populacoes diferentes, e dividir uma pela outra daria um numero sem nome.
        const double T = S.InsertTries > 0 ? static_cast<double>(S.InsertTries) : 0.0;
        auto InsPct = [T](Smile::u32 N) {
            if (T <= 0.0) return QStringLiteral("—");
            return QString::number(100.0 * static_cast<double>(N) / T, 'f', 3) +
                   QStringLiteral("%");
        };
        const QString Probes = T > 0.0
            ? QString::number(static_cast<double>(S.ProbeSum) / T, 'f', 1)
            : QStringLiteral("—");
        // O produtor tem denominador proprio de novo — os caminhos lancados. Terminal em CACHE e
        // o numero que diz se a realimentacao esta viva; tudo em SKY, com o cache ligado ha
        // tempo, e sinal de que o multi-bounce nao comecou.
        const double PathsD = S.Paths > 0 ? static_cast<double>(S.Paths) : 0.0;
        auto TermPct = [PathsD](Smile::u32 N) {
            if (PathsD <= 0.0) return QStringLiteral("—");
            return QString::number(100.0 * static_cast<double>(N) / PathsD, 'f', 1) +
                   QStringLiteral("%");
        };
        const QString Depth = PathsD > 0.0
            ? QString::number(static_cast<double>(S.PathVerts) / PathsD, 'f', 2)
            : QStringLiteral("—");
        return QStringLiteral(
                   "miss: segmento %1 · cone %2 · sem chave %3 · sem amostra %4 · aquecendo %5 · "
                   "refresh %6"
                   "\ninserção: %7 tentativas · balde cheio %8 · contenção %9 · descartadas no "
                   "teto %10 · sondagens %11 (máx %12)"
                   "\nprodutor: %13 caminhos · profundidade %14 (máx %15)"
                   "\nterminal: céu %16 · cache %17 · morto %18 · miss %19 · sem consulta %20 · "
                   "lobo %21 · outro %22")
            .arg(Pct(S.MissShort), Pct(S.MissCone), Pct(S.MissNoEntry), Pct(S.MissEmpty),
                 Pct(S.MissWarming), Pct(S.MissStale))
            .arg(S.InsertTries)
            .arg(InsPct(S.InsertFull), InsPct(S.Contended), InsPct(S.Capped), Probes)
            .arg(S.ProbeMax).arg(S.Paths).arg(Depth).arg(S.PathDepth)
            .arg(TermPct(S.TermSky), TermPct(S.TermCache), TermPct(S.TermKilled),
                 TermPct(S.TermMiss), TermPct(S.TermNoQuery), TermPct(S.TermLobe),
                 TermPct(S.TermOther));
    }

    // Os quatro toggles que governam a telemetria emitem TAMBEM o StatsChanged. Sem isso as linhas
    // de diagnostico so reagiriam ao Timer do painel — que para junto com o cache —, e desligar a
    // instrumentacao deixava numeros antigos na tela por tempo indefinido. Um Q_PROPERTY tem UM
    // sinal de NOTIFY, entao quem muda a validade avisa nos dois canais em vez de a propriedade
    // escutar dois.
    void RenderSettingsBridge::ToggleRadianceCache() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheEnabled(!A->Settings().GetRadianceCacheEnabled());
        emit GISettingsChanged();
        emit StatsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheQuery() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheQuery(!A->Settings().GetRadianceCacheQuery());
        emit GISettingsChanged();
        emit StatsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheAutoWarmup() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheAutoWarmup(!A->Settings().GetRadianceCacheAutoWarmup());
        emit GISettingsChanged();
        emit StatsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheStats() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheStatsEnabled(!A->Settings().GetRadianceCacheStatsEnabled());
        emit GISettingsChanged();
        emit StatsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheStatsDetail() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheStatsDetailEnabled(
            !A->Settings().GetRadianceCacheStatsDetailEnabled());
        emit GISettingsChanged();
        emit StatsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheStatsSource() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheStatsSourceEnabled(
            !A->Settings().GetRadianceCacheStatsSourceEnabled());
        emit GISettingsChanged();
        emit StatsChanged();
    }
    void RenderSettingsBridge::ToggleGISourceDebug() {
        if (!Renderer) return;
        bool Changed = false;
        {
            auto A = Renderer.Lock();
            const bool Before = A->Settings().GetGISourceDebug();
            A->Settings().SetGISourceDebug(!Before);
            // O setter pode RECUSAR (sem produtor vivo), então o que importa é o efeito e não o
            // pedido — notificar uma troca que não houve remontaria o modelo à toa.
            Changed = Before != A->Settings().GetGISourceDebug();
        } // lock SOLTO antes dos sinais: ver abaixo

        emit GISettingsChanged();
        // A lista de alvos da janela de debug é CACHEADA no modelo do viewport e só é relida
        // quando estes sinais saem. Sem isto o alvo entrava e saía do registro do engine — o que
        // já funcionava — e a janela continuava exibindo o modelo antigo e o último preview.
        //
        // Fora do escopo do lock de propósito: o QML relê as propriedades no ato e tenta pegar o
        // mesmo lock do renderer. Emitir com ele na mão é convite a deadlock.
        if (Changed && Viewport) Viewport->NotifyRendererResourcesChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheDedicatedUpdate() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheDedicatedUpdate(
            !A->Settings().GetRadianceCacheDedicatedUpdate());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCacheCompactUpdate() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheCompactUpdate(
            !A->Settings().GetRadianceCacheCompactUpdate());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheUpdateFraction(double V) {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheUpdateFraction(static_cast<float>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheMaxVertices(int V) {
        if (!Renderer || V < 1) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheMaxVertices(static_cast<Smile::u32>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheMinRoughness(double V) {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheMinCacheableRoughness(static_cast<float>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheMinSamples(int V) {
        if (!Renderer || V < 1) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheMinSampleCount(static_cast<Smile::u32>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ToggleRadianceCachePrevTerminal() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheUsePrevTerminal(
            !A->Settings().GetRadianceCacheUsePrevTerminal());
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheCellSize(double V) {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheCellSize(static_cast<float>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheLodDistance(double V) {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheLodDistance(static_cast<float>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::SetRadianceCacheDebugMode(int V) {
        if (!Renderer || V < 0) return;
        auto A = Renderer.Lock();
        A->Settings().SetRadianceCacheDebugMode(static_cast<Smile::u32>(V));
        emit GISettingsChanged();
    }
    void RenderSettingsBridge::ResetRadianceCache() {
        if (!Renderer) return;
        auto A = Renderer.Lock();
        A->Settings().ResetRadianceCache();
        emit GISettingsChanged();
        emit StatsChanged(); // o reset invalida o snapshot; a tela tem de saber no ato
    }
    void RenderSettingsBridge::RefreshRadianceCacheStats() { emit StatsChanged(); }

    void RenderSettingsBridge::ToggleReSTIRGIVisibility() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIVisibility(!Settings.GetGIVisibility());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIFoliageShadows() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // O fan-out p/ os 3 consumidores do HitShading mora no FRenderSettings — era aqui.
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIFoliageShadows(!Settings.GetGIFoliageShadows());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIAdaptiveHysteresis() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // O reset do atlas (e de tudo que se apoia nele) mora no FRenderSettings, dominio
        // GIAccumulation — sem ele o lado "ligado" do A/B comecaria com sondas convergidas
        // pelo lado "desligado".
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIAdaptiveHysteresis(!Settings.GetGIAdaptiveHysteresis());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIAdaptiveRays() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIAdaptiveRays(!Settings.GetGIAdaptiveRays());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetGICascadeCount(int _V) {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // Recria o volume do DDGI (atlas, ProbesTrace, buffers, dispatch). O lock e o mesmo dos
        // outros knobs; a realocacao acontece fora da gravacao do frame.
        RendererAccess->Settings().SetGICascadeCount(static_cast<Smile::u32>(_V < 1 ? 1 : _V));
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIMeasureTerminatorOff() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetGIMeasureTerminatorOff(!Settings.GetGIMeasureTerminatorOff());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleReflectionsCullBackface() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetReflectionsCullBackface(
            !RendererAccess->Settings().GetReflectionsCullBackface());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleReSTIRDI() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseReSTIRDI(!RendererAccess->Settings().GetUseReSTIRDI());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::ToggleGIBackfacePolicy() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        // Pelo Renderer: alem dos reservoirs, o NRD/RR/TAA acumulam sobre o resultado e
        // precisam cair juntos, senao o A/B denoisado compara estado misturado.
        RendererAccess->Settings().SetGIBackfacePolicy(!RendererAccess->Settings().GetGIBackfacePolicy());
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetGISurfaceBiasMax(double _Meters) {
        if (!Renderer) return;
        Renderer->Settings().SetGISurfaceBiasMax(static_cast<float>(qBound(0.0, _Meters, 2.0)));
        emit GISettingsChanged();
    }

    void RenderSettingsBridge::SetGIVolumeFadeProbes(double _Probes) {
        if (!Renderer) return;
        Renderer->Settings().SetGIVolumeFadeProbes(static_cast<float>(qBound(0.0, _Probes, 3.0)));
        emit GISettingsChanged();
    }


    namespace {
        // Tabela unica dos knobs de epsilon: dirige leitura, escrita e a UI. O ponteiro-p/-membro
        // evita 9 getters e 9 setters quase identicos, e a UI e um Repeater sobre esta lista.
        //
        // UiScale = quantas unidades de UI por unidade de mundo (metro). Os offsets aparecem em
        // MILIMETROS porque o sweep interessante e sub-milimetrico (o Lumen usa 0,1-0,5 mm) e ler
        // "0,0002 m" num slider e inutil. MaxAge e contagem de frames, entao escala 1.
        struct FEpsKnob {
            const char* Key;
            const char* Label;
            const char* Unit;
            const char* Hint;
            float Smile::FRayEpsilonProfile::* Field;
            double UiScale;
            double UiMin;
            double UiMax;
            int    Decimals;
        };

        const FEpsKnob kEpsKnobs[] = {
            { "originFloorMin", "Piso do offset de origem", "mm",
              "Somado ao offset ULP em todo raio que sai do G-buffer. 200 mm e o modo legado e a "
              "causa medida das manchas nas cortinas. Varrer ate 0.",
              &Smile::FRayEpsilonProfile::OriginFloorMin, 1000.0, 0.0, 250.0, 2 },
            { "originFloorPerMeter", "Offset por metro de camera", "mm/m",
              "Termo proporcional a distancia da camera. Heuristico: a 50 m ele sozinho da 10 mm. "
              "Varrer ate 0 — a quantizacao do depth justifica ~0,003 mm.",
              &Smile::FRayEpsilonProfile::OriginFloorPerMeter, 1000.0, 0.0, 1.0, 3 },
            { "originAngularMax", "Bias angular (raio rasante)", "mm",
              "Estilo Lumen: raio rasante leva este valor, perpendicular leva 20% dele. 0 = "
              "desligado. O Lumen usa 0,5 mm — mas em coordenadas camera-relative.",
              &Smile::FRayEpsilonProfile::OriginAngularMax, 1000.0, 0.0, 5.0, 3 },
            { "hitShadowRayBias", "Bias do shadow ray (2o hit)", "mm",
              "Desloca a origem das sombras de sol e puntuais no ponto de hit. Independente do "
              "piso acima: baixar so o piso deixa este dominando o contato.",
              &Smile::FRayEpsilonProfile::HitShadowRayBias, 1000.0, 0.0, 250.0, 2 },
            { "shadowRayBiasMin", "Piso do bias do shadow ray", "mm",
              "ATENCAO: em 1 mm ele mascara qualquer sweep do bias acima abaixo disso — o A/B "
              "pareceria nao fazer diferenca pelo motivo errado.",
              &Smile::FRayEpsilonProfile::ShadowRayBiasMin, 1000.0, 0.0, 10.0, 3 },
            { "shadowRayTMin", "TMin do shadow ray", "mm",
              "Soma com o bias: em 10 mm, oclusor a menos de 1 cm do hit fica invisivel mesmo "
              "depois de o bias cair.",
              &Smile::FRayEpsilonProfile::ShadowRayTMin, 1000.0, 0.0, 50.0, 2 },
            { "visRayTMin", "TMin do visibility ray", "mm",
              "Reuso espacial do ReSTIR. Entra no comprimento minimo da conexao, que e derivado "
              "de TMin + margem + folga.",
              &Smile::FRayEpsilonProfile::VisRayTMin, 1000.0, 0.0, 100.0, 2 },
            { "visRayEndMargin", "Margem final do visibility ray", "mm",
              "Para o raio antes da superficie do x2. Tambem entra no comprimento minimo.",
              &Smile::FRayEpsilonProfile::VisRayEndMargin, 1000.0, 0.0, 100.0, 2 },
            { "maxAge", "Idade maxima do reservoir", "frames",
              "Expira a amostra selecionada (RTXDI usa 30). 0 = sem expiracao, que e o estado "
              "atual. O MCap limita o PESO do historico, nao a vida da amostra.",
              &Smile::FRayEpsilonProfile::MaxAge, 1.0, 0.0, 60.0, 0 },
        };
    }

    QVariantList RenderSettingsBridge::GetRayEpsilons() const {
        QVariantList Out;
        if (!Renderer) return Out;
        auto RendererAccess = Renderer.Lock();
        const Smile::FRayEpsilonProfile& P = Renderer->Settings().GetRayEpsilons();
        for (const FEpsKnob& K : kEpsKnobs) {
            QVariantMap M;
            M["key"]      = QString::fromLatin1(K.Key);
            M["label"]    = QString::fromUtf8(K.Label);
            M["unit"]     = QString::fromUtf8(K.Unit);
            M["hint"]     = QString::fromUtf8(K.Hint);
            M["value"]    = static_cast<double>(P.*(K.Field)) * K.UiScale;
            M["min"]      = K.UiMin;
            M["max"]      = K.UiMax;
            M["decimals"] = K.Decimals;
            Out.append(M);
        }
        return Out;
    }

    void RenderSettingsBridge::SetRayEpsilon(const QString& _Key, double _UiValue) {
        if (!Renderer) return;
        for (const FEpsKnob& K : kEpsKnobs) {
            if (_Key != QLatin1String(K.Key)) continue;
            auto RendererAccess = Renderer.Lock();
            Smile::FRayEpsilonProfile P = RendererAccess->Settings().GetRayEpsilons();
            P.*(K.Field) = static_cast<float>(
                qBound(K.UiMin, _UiValue, K.UiMax) / K.UiScale);
            RendererAccess->Settings().SetRayEpsilons(P); // invalida reservoirs + historico do denoiser
            emit GISettingsChanged();
            return;
        }
    }

    void RenderSettingsBridge::ResetRayEpsilons() {
        if (!Renderer) return;
        Renderer->Settings().SetRayEpsilons(Smile::FRayEpsilonProfile{}); // volta aos defaults do header
        emit GISettingsChanged();
    }
    // === Render, upscaler e denoiser ====================================================

    bool RenderSettingsBridge::IsGTAOEnabled() const {
        return Renderer && Renderer->Settings().GetUseAO();
    }

    bool RenderSettingsBridge::IsGTAOHalfRes() const {
        return Renderer && Renderer->Settings().GetAOHalfRes();
    }

    bool RenderSettingsBridge::AreReflectionsEnabled() const {
        return Renderer && Renderer->Settings().GetUseReflections();
    }

    bool RenderSettingsBridge::IsNrdEnabled() const {
        return Renderer && Renderer->Settings().GetUseNrdDenoise();
    }

    int RenderSettingsBridge::GetDenoiserMode() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetDenoiser()) : 0;  // 0=None 1=NRD 2=DLSS RR
    }

    bool RenderSettingsBridge::IsRRAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->Settings().RRAvailable();
    }

    int RenderSettingsBridge::GetUpscalerMode() const {
        return Renderer ? static_cast<int>(Renderer->Settings().GetUpscaler()) : 0;  // 0=None 1=FSR 2=DLSS
    }

    bool RenderSettingsBridge::IsFsrAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->Settings().UpscalerAvailable(Smile::EUpscaler::FSR);
    }

    bool RenderSettingsBridge::IsDlssAvailable() const {
        return Renderer && Renderer->IsInitialized() && Renderer->Settings().UpscalerAvailable(Smile::EUpscaler::DLSS);
    }

    int RenderSettingsBridge::GetUpscalerQuality() const {
        return Renderer ? Renderer->Settings().GetUpscalerQuality() : 0;  // qualidade compartilhada FSR/DLSS
    }

    int RenderSettingsBridge::GetRecommendedUpscalerMode() const {
        if (IsDlssAvailable()) return 2;   // DLSS preferido em NVIDIA
        if (IsFsrAvailable())  return 1;
        return 0;
    }

    QString RenderSettingsBridge::GetRecommendedUpscalerName() const {
        if (IsDlssAvailable()) return QStringLiteral("DLSS");
        if (IsFsrAvailable())  return QStringLiteral("FSR 3.1");
        return QStringLiteral("Sem upscaling");
    }

    double RenderSettingsBridge::GetRenderScale() const {
        return Renderer ? static_cast<double>(Renderer->Settings().GetRenderScale()) : 1.0;
    }

    bool RenderSettingsBridge::IsTAAEnabled() const {
        return Renderer && Renderer->Settings().GetUseTAA();
    }

    bool RenderSettingsBridge::IsFrustumCullingEnabled() const {
        return Renderer && Renderer->Settings().GetFrustumCulling();
    }

    bool RenderSettingsBridge::IsOcclusionCullingEnabled() const {
        return Renderer && Renderer->Settings().GetOcclusionCulling();
    }

    bool RenderSettingsBridge::IsDepthPrepassEnabled() const {
        return Renderer && Renderer->Settings().GetDepthPrepass();
    }

    void RenderSettingsBridge::ToggleGTAO() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseAO(!RendererAccess->Settings().GetUseAO());
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ToggleGTAOHalfRes() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        auto& Settings = RendererAccess->Settings();
        Settings.SetAOHalfRes(!Settings.GetAOHalfRes());
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ToggleReflections() {
        if (!Renderer) return;
        auto RendererAccess = Renderer.Lock();
        RendererAccess->Settings().SetUseReflections(!RendererAccess->Settings().GetUseReflections());
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ToggleNrd() {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetUseNrdDenoise(!_Renderer.Settings().GetUseNrdDenoise());
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyRendererResourcesChanged();
                }
            });
    }

    void RenderSettingsBridge::SetDenoiserMode(int _Mode) {
        if (!Renderer) return;
        // 0=Nenhum 1=NRD 2=DLSS RR. Selecionar RR forca e trava o upscaler em DLSS (o RR faz o upscale);
        // o Renderer cai p/ NRD se o RR nao estiver disponivel (sem NVIDIA/SDK).
        Viewport->EnqueueRendererJob(
            QStringLiteral("denoiser"),
            [_Mode](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetDenoiser(static_cast<Smile::EDenoiser>(_Mode));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyRendererResourcesChanged();
                }
            });
    }

    void RenderSettingsBridge::SetUpscalerMode(int _Mode) {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("upscaler-mode"),
            [_Mode](Smile::Renderer& _Renderer) {
                // 0=None 1=FSR 2=DLSS; cai p/ None se indisponivel.
                _Renderer.Settings().SetUpscaler(static_cast<Smile::EUpscaler>(_Mode));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyRendererResourcesChanged();
                }
            });
    }

    void RenderSettingsBridge::SetUpscalerQuality(int _Quality) {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("upscaler-quality"),
            [_Quality](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetUpscalerQuality(_Quality); // qualidade compartilhada FSR/DLSS
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyRendererResourcesChanged();
                }
            });
    }

    void RenderSettingsBridge::SetRenderScale(double _Scale) {
        if (!Renderer) return;
        Viewport->EnqueueRendererJob(
            QStringLiteral("render-scale"),
            [_Scale](Smile::Renderer& _Renderer) {
                _Renderer.Settings().SetRenderScale(static_cast<float>(_Scale));
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyRendererResourcesChanged();
                }
            });
    }

    void RenderSettingsBridge::SetTAAEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetUseTAA(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::SetFrustumCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetFrustumCulling(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::SetOcclusionCullingEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetOcclusionCulling(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::SetDepthPrepassEnabled(bool _Enabled) {
        if (!Renderer) return;
        Renderer->Settings().SetDepthPrepass(_Enabled);
        emit RenderSettingsChanged();
    }

    void RenderSettingsBridge::ResetRenderSettings() {
        if (!Renderer) return;
        // O padrao segue o backend recomendado (DLSS em NVIDIA, senao FSR, senao nativo), mas em
        // escala 1:1 (tier 0 = 100%): reconstroi/faz AA sem upscale. Bate com o default do Renderer.
        Viewport->EnqueueRendererJob(
            {},
            [](Smile::Renderer& _Renderer) {
                const Smile::EUpscaler Recommended =
                    _Renderer.Settings().UpscalerAvailable(Smile::EUpscaler::DLSS)
                        ? Smile::EUpscaler::DLSS
                        : (_Renderer.Settings().UpscalerAvailable(Smile::EUpscaler::FSR)
                               ? Smile::EUpscaler::FSR
                               : Smile::EUpscaler::None);
                _Renderer.Settings().SetUpscalerQuality(0);
                _Renderer.Settings().SetUpscaler(Recommended);
                _Renderer.Settings().SetUseTAA(true);
                _Renderer.Settings().SetFrustumCulling(true);
                _Renderer.Settings().SetDepthPrepass(false);
                return RenderThread::JobCompletion{ true, {} };
            },
            [this](bool _Success, const QString&) {
                if (_Success) {
                    emit RenderSettingsChanged();
                    if (Viewport) Viewport->NotifyRendererResourcesChanged();
                }
            });
    }
}
