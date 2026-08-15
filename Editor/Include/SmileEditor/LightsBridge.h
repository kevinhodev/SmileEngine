#pragma once

#include <QObject>
#include <QColor>
#include <QString>
#include <QVariantMap>
#include "SmileEditor/RenderThread.h"
// FLight + Vec3 por valor na assinatura do InvalidateLightRegion (a caixa de influencia sai do
// proprio FLight, ver FLight::InfluenceBounds).
#include "Smile/Scene/Light.h"

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Ponte C++<->QML do painel de Luzes (LightsPanel.qml). Le/escreve Scene.Lights() do
    // Renderer direto por meio de RendererHandle, que serializa o acesso com a thread de
    // renderizacao (mesmo racional do TimeOfDayBridge).
    //
    // Sinais: LightsChanged = estrutura da lista mudou (add/remove/duplicar/load) — o QML
    // reconstroi as linhas via `revision` + lightAt(); SelectionChanged = outra luz virou a
    // selecionada (painel OU clique no marker do viewport, sincronizado no Refresh);
    // LightChanged = propriedade da luz selecionada mudou (setter ou gizmo arrastando).
    //
    // Persistencia: <cena>.lights.json ao lado do .sscene (QJson). O load acontece no
    // OnSceneLoaded (MainWindow chama apos LoadCookedScene); o save e explicito (botao do
    // painel) com flag `dirty` pra sinalizar mudancas nao salvas.
    class LightsBridge : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool available READ Available NOTIFY AvailableChanged)
        Q_PROPERTY(int count READ Count NOTIFY LightsChanged)
        Q_PROPERTY(int revision READ Revision NOTIFY LightsChanged)
        Q_PROPERTY(int selectedIndex READ SelectedIndex WRITE SelectLight NOTIFY SelectionChanged)
        Q_PROPERTY(bool dirty READ Dirty NOTIFY DirtyChanged)
        Q_PROPERTY(bool hasSceneFile READ HasSceneFile NOTIFY LightsChanged)
        // Propriedades da luz SELECIONADA (leituras devolvem defaults sem selecao).
        Q_PROPERTY(QString name READ Name WRITE SetName NOTIFY LightChanged)
        Q_PROPERTY(int lightType READ LightType NOTIFY LightChanged) // 0=point, 1=spot
        Q_PROPERTY(bool lightEnabled READ LightEnabled WRITE SetLightEnabled NOTIFY LightChanged)
        Q_PROPERTY(bool castShadows READ CastShadows WRITE SetCastShadows NOTIFY LightChanged)
        Q_PROPERTY(QColor color READ Color WRITE SetColor NOTIFY LightChanged)
        Q_PROPERTY(double intensity READ Intensity WRITE SetIntensity NOTIFY LightChanged)
        Q_PROPERTY(double radius READ Radius WRITE SetRadius NOTIFY LightChanged)
        Q_PROPERTY(double sourceRadius READ SourceRadius WRITE SetSourceRadius NOTIFY LightChanged)
        // Peso da luz so no indireto (GI/reflexoes/DDGI). 0 tira a luz do RT sem tocar no raster.
        Q_PROPERTY(double rtWeight READ RTWeight WRITE SetRTWeight NOTIFY LightChanged)
        Q_PROPERTY(double innerConeDeg READ InnerConeDeg WRITE SetInnerConeDeg NOTIFY LightChanged)
        Q_PROPERTY(double outerConeDeg READ OuterConeDeg WRITE SetOuterConeDeg NOTIFY LightChanged)
        Q_PROPERTY(double posX READ PosX WRITE SetPosX NOTIFY LightChanged)
        Q_PROPERTY(double posY READ PosY WRITE SetPosY NOTIFY LightChanged)
        Q_PROPERTY(double posZ READ PosZ WRITE SetPosZ NOTIFY LightChanged)
        Q_PROPERTY(double spotAzimuthDeg READ SpotAzimuthDeg WRITE SetSpotAzimuthDeg NOTIFY LightChanged)
        Q_PROPERTY(double spotElevationDeg READ SpotElevationDeg WRITE SetSpotElevationDeg NOTIFY LightChanged)

    public:
        explicit LightsBridge(QObject* parent = nullptr);

        void SetRenderer(RendererHandle R);            // MainWindow chama no RendererInitialized
        void OnSceneLoaded(const QString& ScenePath, bool Additive); // apos LoadCookedScene OK

        bool    Available() const { return static_cast<bool>(Renderer); }
        int     Count() const;
        int     Revision() const { return Rev; }
        int     SelectedIndex() const;
        bool    Dirty() const { return DirtyFlag; }
        bool    HasSceneFile() const { return !JsonPath.isEmpty(); }

        QString Name() const;
        int     LightType() const;
        bool    LightEnabled() const;
        bool    CastShadows() const;
        double  RTWeight() const;
        QColor  Color() const;
        double  Intensity() const;
        double  Radius() const;
        double  SourceRadius() const;
        double  InnerConeDeg() const;
        double  OuterConeDeg() const;
        double  PosX() const;
        double  PosY() const;
        double  PosZ() const;
        double  SpotAzimuthDeg() const;
        double  SpotElevationDeg() const;

        void SetName(const QString& V);
        void SetLightEnabled(bool V);
        void SetCastShadows(bool V);
        void SetRTWeight(double V);
        void SetColor(const QColor& V);
        void SetIntensity(double V);
        void SetRadius(double V);
        void SetSourceRadius(double V);
        void SetInnerConeDeg(double V);
        void SetOuterConeDeg(double V);
        void SetPosX(double V);
        void SetPosY(double V);
        void SetPosZ(double V);
        void SetSpotAzimuthDeg(double V);
        void SetSpotElevationDeg(double V);

        // Dados de uma linha da lista (delegate do QML): name, type, enabled, color.
        Q_INVOKABLE QVariantMap lightAt(int index) const;
        Q_INVOKABLE void addLight(int type);          // 0=point, 1=spot; nasce na frente da camera
        Q_INVOKABLE void removeLight(int index);
        Q_INVOKABLE void duplicateLight(int index);
        Q_INVOKABLE void selectLight(int index);
        Q_INVOKABLE void toggleLightEnabled(int index); // toggle direto na linha da lista
        Q_INVOKABLE void placeAtCamera();             // selecionada -> frente da camera
        Q_INVOKABLE bool saveLights();
        Q_INVOKABLE void closePanel() { emit CloseRequested(); }

    public slots:
        void Refresh(); // FrameReady: sincroniza selecao do viewport e arraste do gizmo

    signals:
        void AvailableChanged();
        void LightsChanged();
        void SelectionChanged();
        void LightChanged();
        void DirtyChanged();
        void CloseRequested();

    private:
        void SelectLight(int index) { selectLight(index); } // WRITE da Q_PROPERTY
        void Touch(bool Structure); // rev++/dirty + sinais (Structure = lista mudou de forma)

        // Invalidacao dos caches de GI depois de editar uma luz. `OldMin/OldMax` e a caixa de
        // influencia de ANTES da edicao — capture-a no setter antes de mexer no campo. Nos
        // setters que nao mexem em posicao nem raio ela e igual a de depois, e passar as duas
        // continua correto (o FDDGI une caixas).
        //
        // O Touch() ao lado dela cuida so de UI e persistencia: quem consome a luz no mundo —
        // atlas do DDGI, reservoirs, ReGIR, reflexoes, cache de radiancia — nao ouve sinal de Qt.
        void InvalidateLightRegion(const Smile::FLight& L,
                                   const Smile::Vec3& OldMin, const Smile::Vec3& OldMax);
        bool LoadLights();          // le o JsonPath e ADICIONA na cena

        RendererHandle Renderer;
        QString JsonPath;           // <cena>.lights.json da cena principal (nao-aditiva)
        bool    DirtyFlag = false;
        int     Rev = 0;
        int     PointSeq = 0;       // sequencial pro nome default ("Point 3")
        int     SpotSeq = 0;

        // Estado espelhado p/ o Refresh detectar mudanca externa (clique/gizmo do viewport).
        int    CachedSelected = -1;
        double CachedPos[3] = { 0.0, 0.0, 0.0 };
    };
}
