#include "SmileEditor/LightsBridge.h"
#include "SmileEditor/JsonSidecar.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/Logger.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace SmileEditor {
    namespace {
        constexpr double kToRad = 3.14159265358979 / 180.0;
        constexpr double kToDeg = 180.0 / 3.14159265358979;

        // Direcao <-> azimute/elevacao (graus). Mesmo convencao da camera/TOD:
        // az 0 = +Z, el +90 = pra cima; spot default (0,-1,0) = el -90.
        Smile::Vec3 DirFromAzEl(double AzDeg, double ElDeg) {
            const double Az = AzDeg * kToRad, El = ElDeg * kToRad;
            return { (float)(std::cos(El) * std::sin(Az)),
                     (float)(std::sin(El)),
                     (float)(std::cos(El) * std::cos(Az)) };
        }

        QJsonArray Vec3ToJson(const Smile::Vec3& V) {
            return QJsonArray{ V.X, V.Y, V.Z };
        }
        Smile::Vec3 Vec3FromJson(const QJsonValue& V, const Smile::Vec3& Fallback) {
            const QJsonArray A = V.toArray();
            if (A.size() != 3) return Fallback;
            return { (float)A[0].toDouble(), (float)A[1].toDouble(), (float)A[2].toDouble() };
        }
    }

    LightsBridge::LightsBridge(QObject* _Parent) : QObject(_Parent) {}

    void LightsBridge::SetRenderer(RendererHandle _Renderer) {
        Renderer = _Renderer;
        emit AvailableChanged();
        emit LightsChanged();
        emit SelectionChanged();
        emit LightChanged();
    }

    // ---- Acesso ----
    class LockedLights {
    public:
        explicit LockedLights(const RendererHandle& _Renderer)
            : Access(_Renderer ? _Renderer.Lock() : RendererHandle::Access{}) {
            if (Access) Value = &Access->GetScene().Lights();
        }

        explicit operator bool() const { return Value != nullptr; }
        std::vector<Smile::FLight>* operator->() const { return Value; }
        Smile::FLight& operator[](size_t _Index) const { return (*Value)[_Index]; }
        size_t size() const { return Value ? Value->size() : 0; }
        auto begin() const { return Value ? Value->begin() : Empty().begin(); }
        auto end() const { return Value ? Value->end() : Empty().end(); }

    private:
        static std::vector<Smile::FLight>& Empty() {
            static std::vector<Smile::FLight> EmptyLights;
            return EmptyLights;
        }

        RendererHandle::Access      Access;
        std::vector<Smile::FLight>* Value = nullptr;
    };

    class LockedSelectedLight {
    public:
        explicit LockedSelectedLight(const RendererHandle& _Renderer)
            : Access(_Renderer ? _Renderer.Lock() : RendererHandle::Access{}) {
            if (!Access) return;
            auto& Lights = Access->GetScene().Lights();
            const int Index = Access->GetSelectedLight();
            if (Index >= 0 && Index < static_cast<int>(Lights.size()))
                Value = &Lights[static_cast<size_t>(Index)];
        }

        explicit operator bool() const { return Value != nullptr; }
        Smile::FLight* operator->() const { return Value; }
        // Sob o MESMO Access do operator-> acima: os setters que invalidam GI por regiao passam
        // a luz por referencia p/ o InvalidateLightRegion. So chame com o bool ja testado — como
        // o operator->, nao ha checagem aqui.
        Smile::FLight& operator*() const { return *Value; }

    private:
        RendererHandle::Access Access;
        Smile::FLight*         Value = nullptr;
    };

    static LockedLights LightsOf(const RendererHandle& _R) { return LockedLights(_R); }
    static LockedSelectedLight SelOf(const RendererHandle& _R) {
        return LockedSelectedLight(_R);
    }

    int LightsBridge::Count() const         { return (int)LightsOf(Renderer).size(); }
    int LightsBridge::SelectedIndex() const {
        if (!Renderer) return -1;
        const int I = Renderer->GetSelectedLight();
        return (I >= 0 && I < Count()) ? I : -1;
    }

    QString LightsBridge::Name() const {
        const auto L = SelOf(Renderer);
        return L ? QString::fromStdString(L->Name) : QString();
    }
    int LightsBridge::LightType() const {
        const auto L = SelOf(Renderer);
        return (L && L->Type == Smile::ELightType::Spot) ? 1 : 0;
    }
    bool LightsBridge::LightEnabled() const {
        const auto L = SelOf(Renderer);
        return L ? L->Enabled : false;
    }
    bool LightsBridge::CastShadows() const {
        const auto L = SelOf(Renderer);
        return L ? L->CastShadows : true;
    }
    QColor LightsBridge::Color() const {
        const auto L = SelOf(Renderer);
        if (!L) return QColor(255, 255, 255);
        return QColor::fromRgbF(std::clamp(L->Color.X, 0.0f, 1.0f),
                                std::clamp(L->Color.Y, 0.0f, 1.0f),
                                std::clamp(L->Color.Z, 0.0f, 1.0f));
    }
    double LightsBridge::Intensity() const    { const auto L = SelOf(Renderer); return L ? L->Intensity : 0.0; }
    double LightsBridge::Radius() const       { const auto L = SelOf(Renderer); return L ? L->AttenuationRadius : 0.0; }
    double LightsBridge::SourceRadius() const { const auto L = SelOf(Renderer); return L ? L->SourceRadius : 0.05; }
    double LightsBridge::RTWeight() const     { const auto L = SelOf(Renderer); return L ? L->RTWeight : 1.0; }
    double LightsBridge::InnerConeDeg() const { const auto L = SelOf(Renderer); return L ? L->InnerConeDeg : 25.0; }
    double LightsBridge::OuterConeDeg() const { const auto L = SelOf(Renderer); return L ? L->OuterConeDeg : 40.0; }
    double LightsBridge::PosX() const         { const auto L = SelOf(Renderer); return L ? L->Position.X : 0.0; }
    double LightsBridge::PosY() const         { const auto L = SelOf(Renderer); return L ? L->Position.Y : 0.0; }
    double LightsBridge::PosZ() const         { const auto L = SelOf(Renderer); return L ? L->Position.Z : 0.0; }

    double LightsBridge::SpotAzimuthDeg() const {
        const auto L = SelOf(Renderer);
        if (!L) return 0.0;
        double Az = std::atan2((double)L->Direction.X, (double)L->Direction.Z) * kToDeg;
        return Az < 0.0 ? Az + 360.0 : Az;
    }
    double LightsBridge::SpotElevationDeg() const {
        const auto L = SelOf(Renderer);
        if (!L) return -90.0;
        const auto D = L->Direction.NormalizedSafe(Smile::Vec3{ 0.0f, -1.0f, 0.0f });
        return std::asin(std::clamp((double)D.Y, -1.0, 1.0)) * kToDeg;
    }

    // ---- Setters da selecionada ----
    void LightsBridge::Touch(bool _Structure) {
        ++Rev;
        if (!DirtyFlag) { DirtyFlag = true; emit DirtyChanged(); }
        if (_Structure) emit LightsChanged();
        emit LightChanged();
    }

    void LightsBridge::InvalidateLightRegion(const Smile::FLight& _L,
                                             const Smile::Vec3& _OldMin,
                                             const Smile::Vec3& _OldMax) {
        if (!Renderer) return;
        Smile::Vec3 NewMin, NewMax;
        _L.InfluenceBounds(NewMin, NewMax);
        // Duas caixas em vez de uma uniao na mao: o FDDGI une chamadas dentro da janela e a
        // uniao dele e CRUA, entao mudar o raio de 2 p/ 40 m nao deixa a caixa antiga de fora
        // nem paga padding duas vezes.
        // Funil de TODA edicao de luz, e nenhuma delas move geometria: radiometrico sempre.
        Renderer->NotifyGIRegionChanged(_OldMin, _OldMax, Smile::EGIRegionChange::Radiometric);
        Renderer->NotifyGIRegionChanged(NewMin, NewMax, Smile::EGIRegionChange::Radiometric);
        // Coalescido: um arraste de slider de intensidade chama isto a cada tick, e cada
        // NotifySceneContentChanged imediato limparia reservoir/ReGIR/cache no meio do gesto.
        Renderer->Settings().MarkSceneContentDirty();
    }

    void LightsBridge::SetName(const QString& _V) {
        auto L = SelOf(Renderer); if (!L) return;
        const std::string S = _V.trimmed().toStdString();
        if (S.empty() || L->Name == S) return;
        L->Name = S;
        Touch(true); // nome aparece na lista
    }
    void LightsBridge::SetLightEnabled(bool _V) {
        auto L = SelOf(Renderer); if (!L || L->Enabled == _V) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Enabled = _V;
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(true); // estado aparece na lista
    }
    // SEM invalidacao, e nao por esquecimento: a lista de luzes do GI (Renderer.cpp, montagem do
    // FGPULightGI) nao le CastShadows — o hit do indireto sempre dispara shadow ray. Este campo
    // so escolhe slot no atlas 2D / cube array do raster, que nao acumula entre frames.
    void LightsBridge::SetCastShadows(bool _V) {
        auto L = SelOf(Renderer); if (!L || L->CastShadows == _V) return;
        L->CastShadows = _V;
        Touch(false);
    }
    void LightsBridge::SetColor(const QColor& _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Color = { (float)_V.redF(), (float)_V.greenF(), (float)_V.blueF() };
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(true); // dot de cor na lista
    }
    void LightsBridge::SetIntensity(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Intensity = (float)std::max(_V, 0.0);
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    // Unico setter em que a caixa ANTIGA e a NOVA sao mesmo diferentes: encolher o raio deixa
    // sondas que estavam iluminadas fora da caixa nova, e sem a antiga elas guardam a luz.
    void LightsBridge::SetRadius(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->AttenuationRadius = (float)std::clamp(_V, 0.1, 500.0);
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    void LightsBridge::SetSourceRadius(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        // Nao muda a caixa, mas muda a ENERGIA: o SourceRadius e o piso da distancia no
        // inverse-square, entao entra no FGPULightGI e no que a sonda mede.
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->SourceRadius = (float)std::clamp(_V, 0.01, 2.0);
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    void LightsBridge::SetRTWeight(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        // Teto em 1: isto e para REMOVER duplicata de emissivo, nao para inflar o indireto acima do
        // que a luz entrega no direto. Quem quiser mais energia sobe a Intensity, que vale nos dois.
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->RTWeight = (float)std::clamp(_V, 0.0, 1.0);
        // Quem consome este peso — DDGI, ReSTIR GI e reflexoes — ACUMULOU energia da luz antiga,
        // e o atlas seguraria essa energia por muitos frames: o slider pareceria inerte e a
        // calibracao seria contra um estado que o usuario ja mandou embora.
        //
        // Era MarkIndirectLightingDirty (dominio SkyRadiance), que carrega DDGIAtlas e portanto
        // derrubava o atlas INTEIRO — a piscada de GI a cada tique do slider. Trocado pela via
        // por regiao + SceneContent: a caixa de influencia cobre o DDGI com precisao e o
        // SceneContent ainda alcanca MAIS caches que o SkyRadiance (ReGIR e o cache de radiancia
        // tambem guardam energia de luz puntual). Este e o caminho de toda edicao de luz agora.
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    void LightsBridge::SetInnerConeDeg(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->InnerConeDeg = (float)std::clamp(_V, 0.0, 89.0);
        L->OuterConeDeg = std::max(L->OuterConeDeg, L->InnerConeDeg); // inner <= outer sempre
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    void LightsBridge::SetOuterConeDeg(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->OuterConeDeg = (float)std::clamp(_V, 1.0, 89.0);
        L->InnerConeDeg = std::min(L->InnerConeDeg, L->OuterConeDeg);
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    // Posicao por campo numerico: a caixa se desloca, entao a antiga entra igual ao arraste do
    // gizmo. Os tres eixos sao setters separados, e mexer em X e depois em Y da duas chamadas —
    // a uniao dentro do FDDGI cobre o caminho, nao so as pontas.
    void LightsBridge::SetPosX(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Position.X = (float)_V;
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    void LightsBridge::SetPosY(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Position.Y = (float)_V;
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    void LightsBridge::SetPosZ(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Position.Z = (float)_V;
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    // Direcao do spot: a esfera de influencia nao muda, mas o cone aponta p/ outro lugar e as
    // sondas dos dois lados reavaliam. A caixa conservadora ja cobre os dois.
    void LightsBridge::SetSpotAzimuthDeg(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Direction = DirFromAzEl(_V, SpotElevationDeg());
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }
    void LightsBridge::SetSpotElevationDeg(double _V) {
        auto L = SelOf(Renderer); if (!L) return;
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Direction = DirFromAzEl(SpotAzimuthDeg(), std::clamp(_V, -90.0, 90.0));
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }

    // ---- Lista ----
    QVariantMap LightsBridge::lightAt(int _Index) const {
        QVariantMap M;
        const auto L = LightsOf(Renderer);
        if (_Index < 0 || _Index >= (int)L.size()) return M;
        const Smile::FLight& Light = L[(size_t)_Index];
        M[QStringLiteral("name")]    = QString::fromStdString(Light.Name);
        M[QStringLiteral("type")]    = (Light.Type == Smile::ELightType::Spot) ? 1 : 0;
        M[QStringLiteral("enabled")] = Light.Enabled;
        M[QStringLiteral("color")]   = QColor::fromRgbF(std::clamp(Light.Color.X, 0.0f, 1.0f),
                                                        std::clamp(Light.Color.Y, 0.0f, 1.0f),
                                                        std::clamp(Light.Color.Z, 0.0f, 1.0f));
        return M;
    }

    void LightsBridge::addLight(int _Type) {
        if (!Renderer) return;

        Smile::FLight L;
        if (_Type == 1) {
            L.Type = Smile::ELightType::Spot;
            L.Name = "Spot " + std::to_string(++SpotSeq);
            L.Color = { 1.0f, 0.95f, 0.85f };
            L.Intensity = 60.0f;
            L.AttenuationRadius = 15.0f;
            L.Direction = { 0.0f, -1.0f, 0.0f };
        } else {
            L.Type = Smile::ELightType::Point;
            L.Name = "Point " + std::to_string(++PointSeq);
            L.Color = { 1.0f, 0.85f, 0.65f };
            L.Intensity = 20.0f;
            L.AttenuationRadius = 10.0f;
        }

        // Nasce na frente da camera (mesma convencao de forward da FCamera), um pouco acima
        // do olhar pro marker nao nascer atras do crosshair.
        const double Pitch = Renderer->GetPitch() * kToRad;
        const double Yaw   = Renderer->GetYaw()   * kToRad;
        const Smile::Vec3 Fwd = { (float)(std::cos(Pitch) * std::sin(Yaw)),
                                  (float)std::sin(Pitch),
                                  (float)(std::cos(Pitch) * std::cos(Yaw)) };
        L.Position = Renderer->GetCameraPos() + Fwd * 5.0f;

        auto Lights = LightsOf(Renderer);
        Lights->push_back(L);
        Renderer->SetSelectedLight((int)Lights.size() - 1);
        Renderer->ClearSelection(); // selecao de luz e de renderavel sao exclusivas
        // Luz nova ilumina uma regiao que ate agora estava sem ela: mesma invalidacao de uma
        // edicao, so que sem caixa antiga (a "antiga" e a propria nova).
        Smile::Vec3 BornMin, BornMax; L.InfluenceBounds(BornMin, BornMax);
        InvalidateLightRegion(L, BornMin, BornMax);
        Touch(true);
        emit SelectionChanged();
    }

    void LightsBridge::removeLight(int _Index) {
        if (!Renderer) return;
        auto Lights = LightsOf(Renderer);
        if (_Index < 0 || _Index >= (int)Lights.size()) return;
        // ANTES do erase: depois dele nao ha de onde tirar a caixa, e sem ela a energia da luz
        // apagada fica no atlas (mesmo motivo do Renderer::RemoveRenderable).
        Smile::Vec3 GoneMin, GoneMax;
        Lights[(size_t)_Index].InfluenceBounds(GoneMin, GoneMax);
        Lights->erase(Lights->begin() + _Index);
        Renderer->NotifyGIRegionChanged(GoneMin, GoneMax, Smile::EGIRegionChange::Radiometric);
        Renderer->Settings().MarkSceneContentDirty();

        int Sel = Renderer->GetSelectedLight();
        if (Sel == _Index)     Sel = -1;
        else if (Sel > _Index) --Sel;
        Renderer->SetSelectedLight(Sel);

        Touch(true);
        emit SelectionChanged();
    }

    void LightsBridge::duplicateLight(int _Index) {
        if (!Renderer) return;
        auto Lights = LightsOf(Renderer);
        if (_Index < 0 || _Index >= (int)Lights.size()) return;
        Smile::FLight Copy = Lights[(size_t)_Index];
        Copy.Id = 0; // copia nasce SEM identidade: o renderer atribui uma nova no proximo
                     // frame. Herdar o Id faria as duas luzes disputarem o mesmo slot de
                     // shadow map (uma piscaria em cima da outra).
        Copy.Name += " (copia)";
        Copy.Position.X += 1.0f; // desloca pro marker nao nascer em cima do original
        Lights->push_back(Copy);
        Renderer->SetSelectedLight((int)Lights.size() - 1);
        Renderer->ClearSelection();
        Smile::Vec3 CopyMin, CopyMax; Copy.InfluenceBounds(CopyMin, CopyMax);
        InvalidateLightRegion(Copy, CopyMin, CopyMax);
        Touch(true);
        emit SelectionChanged();
    }

    void LightsBridge::selectLight(int _Index) {
        if (!Renderer) return;
        const int Clamped = (_Index >= 0 && _Index < Count()) ? _Index : -1;
        if (Renderer->GetSelectedLight() == Clamped) return;
        Renderer->SetSelectedLight(Clamped);
        if (Clamped >= 0) Renderer->ClearSelection();
        emit SelectionChanged();
        emit LightChanged();
    }

    void LightsBridge::toggleLightEnabled(int _Index) {
        if (!Renderer) return;
        auto Lights = LightsOf(Renderer);
        if (_Index < 0 || _Index >= (int)Lights.size()) return;
        // Mesmo evento do SetLightEnabled, por outro caminho de UI (o olho da linha da lista, que
        // nao passa pela luz SELECIONADA). Sem isto, a mesma acao invalidaria ou nao dependendo
        // de onde o usuario clicou.
        Smile::FLight& L = Lights[(size_t)_Index];
        Smile::Vec3 OldMin, OldMax; L.InfluenceBounds(OldMin, OldMax);
        L.Enabled = !L.Enabled;
        InvalidateLightRegion(L, OldMin, OldMax);
        Touch(true);
    }

    void LightsBridge::placeAtCamera() {
        auto L = SelOf(Renderer); if (!L) return;
        const double Pitch = Renderer->GetPitch() * kToRad;
        const double Yaw   = Renderer->GetYaw()   * kToRad;
        const Smile::Vec3 Fwd = { (float)(std::cos(Pitch) * std::sin(Yaw)),
                                  (float)std::sin(Pitch),
                                  (float)(std::cos(Pitch) * std::cos(Yaw)) };
        Smile::Vec3 OldMin, OldMax; L->InfluenceBounds(OldMin, OldMax);
        L->Position = Renderer->GetCameraPos() + Fwd * 5.0f;
        if (L->Type == Smile::ELightType::Spot) L->Direction = Fwd; // spot aponta pro olhar
        // Teleporte, nao arraste: a caixa antiga pode estar do outro lado da cena, e e por isso
        // que ela precisa entrar junto com a nova.
        InvalidateLightRegion(*L, OldMin, OldMax);
        Touch(false);
    }

    // ---- Persistencia ----
    void LightsBridge::OnSceneLoaded(const QString& _ScenePath, bool _Additive) {
        if (!_Additive) {
            // Carga de substituicao: a engine limpou Scene.Lights(); o JSON da cena nova define
            // o caminho de save e repovoa. Contadores de nome recomecam.
            const QFileInfo Info(_ScenePath);
            JsonPath = Info.path() + "/" + Info.completeBaseName() + ".lights.json";
            PointSeq = SpotSeq = 0;
            LoadLights();
            if (DirtyFlag) { DirtyFlag = false; emit DirtyChanged(); }
        } else {
            // Carga aditiva: mantem o caminho da cena principal; soma as luzes do JSON da
            // cena adicionada, se existir.
            const QFileInfo Info(_ScenePath);
            const QString AddPath = Info.path() + "/" + Info.completeBaseName() + ".lights.json";
            const QString Keep = JsonPath;
            JsonPath = AddPath;
            const bool Added = LoadLights();
            JsonPath = Keep;
            if (Added && !DirtyFlag) { DirtyFlag = true; emit DirtyChanged(); } // salva no principal
        }
        emit LightsChanged();
        emit SelectionChanged();
        emit LightChanged();
    }

    bool LightsBridge::LoadLights() {
        if (!Renderer || JsonPath.isEmpty()) return false;
        QFile File(JsonPath);
        if (!File.exists()) return false;
        if (!File.open(QIODevice::ReadOnly)) {
            Smile::LogWarning("Luzes: falha ao abrir " + JsonPath.toStdString());
            return false;
        }
        QJsonParseError Err{};
        const QJsonDocument Doc = QJsonDocument::fromJson(File.readAll(), &Err);
        if (Err.error != QJsonParseError::NoError || !Doc.isObject()) {
            Smile::LogWarning("Luzes: JSON invalido em " + JsonPath.toStdString() + " — " +
                              Err.errorString().toStdString());
            return false;
        }

        auto Lights = LightsOf(Renderer);
        const QJsonArray Arr = Doc.object().value(QStringLiteral("lights")).toArray();
        int Loaded = 0;
        for (const QJsonValue& V : Arr) {
            const QJsonObject O = V.toObject();
            Smile::FLight L;
            L.Name      = O.value(QStringLiteral("name")).toString(QStringLiteral("Luz")).toStdString();
            L.Type      = O.value(QStringLiteral("type")).toString() == QStringLiteral("spot")
                        ? Smile::ELightType::Spot : Smile::ELightType::Point;
            L.Position  = Vec3FromJson(O.value(QStringLiteral("position")),  L.Position);
            L.Direction = Vec3FromJson(O.value(QStringLiteral("direction")), L.Direction);
            L.Color     = Vec3FromJson(O.value(QStringLiteral("color")),     L.Color);
            L.Intensity         = (float)O.value(QStringLiteral("intensity")).toDouble(L.Intensity);
            L.AttenuationRadius = (float)O.value(QStringLiteral("radius")).toDouble(L.AttenuationRadius);
            L.SourceRadius      = (float)O.value(QStringLiteral("sourceRadius")).toDouble(L.SourceRadius);
            L.InnerConeDeg      = (float)O.value(QStringLiteral("innerConeDeg")).toDouble(L.InnerConeDeg);
            L.OuterConeDeg      = (float)O.value(QStringLiteral("outerConeDeg")).toDouble(L.OuterConeDeg);
            L.Enabled           = O.value(QStringLiteral("enabled")).toBool(true);
            L.CastShadows       = O.value(QStringLiteral("castShadows")).toBool(true);
            // Default 1.0: cena salva antes deste campo existir continua com a luz cheia no RT.
            // Clamp TAMBEM aqui: o setter limita a [0,1], mas o JSON e fronteira de entrada — um
            // arquivo editado a mao reintroduziria peso > 1 e inflaria o indireto acima do direto,
            // que e exatamente o contrato que o teto existe p/ garantir. Negativo viraria energia
            // negativa no hit.
            L.RTWeight          = std::clamp(
                (float)O.value(QStringLiteral("rtWeight")).toDouble(1.0), 0.0f, 1.0f);
            Lights->push_back(L);
            ++Loaded;
            // Mantem os sequenciais de nome a frente dos "Point N"/"Spot N" carregados.
            if (L.Type == Smile::ELightType::Spot) ++SpotSeq; else ++PointSeq;
        }
        if (Loaded > 0)
            Smile::LogInfo("Luzes: " + std::to_string(Loaded) + " carregadas de " +
                           QFileInfo(JsonPath).fileName().toStdString());
        return Loaded > 0;
    }

    bool LightsBridge::saveLights() {
        if (!Renderer || JsonPath.isEmpty()) return false;

        QJsonArray Arr;
        for (const Smile::FLight& L : LightsOf(Renderer)) {
            QJsonObject O;
            O[QStringLiteral("name")]         = QString::fromStdString(L.Name);
            O[QStringLiteral("type")]         = (L.Type == Smile::ELightType::Spot)
                                              ? QStringLiteral("spot") : QStringLiteral("point");
            O[QStringLiteral("position")]     = Vec3ToJson(L.Position);
            O[QStringLiteral("direction")]    = Vec3ToJson(L.Direction);
            O[QStringLiteral("color")]        = Vec3ToJson(L.Color);
            O[QStringLiteral("intensity")]    = L.Intensity;
            O[QStringLiteral("radius")]       = L.AttenuationRadius;
            O[QStringLiteral("sourceRadius")] = L.SourceRadius;
            O[QStringLiteral("innerConeDeg")] = L.InnerConeDeg;
            O[QStringLiteral("outerConeDeg")] = L.OuterConeDeg;
            O[QStringLiteral("enabled")]      = L.Enabled;
            O[QStringLiteral("castShadows")]  = L.CastShadows;
            O[QStringLiteral("rtWeight")]     = L.RTWeight;
            Arr.append(O);
        }
        QJsonObject Root;
        Root[QStringLiteral("version")] = 1;
        Root[QStringLiteral("lights")]  = Arr;

        if (!WriteJsonSidecar(JsonPath, Root, "Luzes")) return false;
        Smile::LogInfo("Luzes: " + std::to_string(Arr.size()) + " salvas em " +
                       QFileInfo(JsonPath).fileName().toStdString());
        if (DirtyFlag) { DirtyFlag = false; emit DirtyChanged(); }
        return true;
    }

    // ---- Sincronizacao por frame ----
    void LightsBridge::Refresh() {
        if (!Renderer) return;

        // Selecao mudou por fora (clique no marker do viewport / pick de renderavel limpou).
        const int Sel = SelectedIndex();
        if (Sel != CachedSelected) {
            CachedSelected = Sel;
            const auto L = SelOf(Renderer);
            if (L) CacheSelectedPose(*L);
            emit SelectionChanged();
            emit LightChanged();
            return;
        }

        // Gizmo arrastando a luz: reflete no painel e marca dirty. DIRECAO junto com a posicao —
        // o gizmo de rotacao aponta o cone do spot, e vigiar so a posicao deixava o painel
        // mostrando o azimute/elevacao velhos e, pior, o sidecar de luzes NAO ficava sujo: a
        // rotacao se perdia sem aviso ao fechar o editor.
        const auto L = SelOf(Renderer);
        if (L && !PoseMatchesCache(*L)) {
            CacheSelectedPose(*L);
            if (!DirtyFlag) { DirtyFlag = true; emit DirtyChanged(); }
            emit LightChanged();
        }
    }

    void LightsBridge::CacheSelectedPose(const Smile::FLight& _L) {
        CachedPos[0] = _L.Position.X;
        CachedPos[1] = _L.Position.Y;
        CachedPos[2] = _L.Position.Z;
        CachedDir[0] = _L.Direction.X;
        CachedDir[1] = _L.Direction.Y;
        CachedDir[2] = _L.Direction.Z;
    }

    bool LightsBridge::PoseMatchesCache(const Smile::FLight& _L) const {
        return std::abs(_L.Position.X - CachedPos[0]) <= 1e-5 &&
               std::abs(_L.Position.Y - CachedPos[1]) <= 1e-5 &&
               std::abs(_L.Position.Z - CachedPos[2]) <= 1e-5 &&
               std::abs(_L.Direction.X - CachedDir[0]) <= 1e-5 &&
               std::abs(_L.Direction.Y - CachedDir[1]) <= 1e-5 &&
               std::abs(_L.Direction.Z - CachedDir[2]) <= 1e-5;
    }
}
