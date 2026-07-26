#include "SmileEditor/LightsBridge.h"
#include "Smile/Graphics/Renderer.h"
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

    void LightsBridge::SetRenderer(Smile::Renderer* _Renderer) {
        Renderer = _Renderer;
        emit AvailableChanged();
        emit LightsChanged();
        emit SelectionChanged();
        emit LightChanged();
    }

    // ---- Acesso ----
    static std::vector<Smile::FLight>& LightsOf(Smile::Renderer* _R) {
        static std::vector<Smile::FLight> Empty;
        return _R ? _R->GetScene().Lights() : Empty;
    }

    // Luz selecionada ou nullptr (sem renderer / indice invalido).
    static Smile::FLight* SelOf(Smile::Renderer* _R) {
        if (!_R) return nullptr;
        auto& L = _R->GetScene().Lights();
        const int I = _R->GetSelectedLight();
        return (I >= 0 && I < (int)L.size()) ? &L[(size_t)I] : nullptr;
    }

    int LightsBridge::Count() const         { return (int)LightsOf(Renderer).size(); }
    int LightsBridge::SelectedIndex() const {
        if (!Renderer) return -1;
        const int I = Renderer->GetSelectedLight();
        return (I >= 0 && I < Count()) ? I : -1;
    }

    QString LightsBridge::Name() const {
        const auto* L = SelOf(Renderer);
        return L ? QString::fromStdString(L->Name) : QString();
    }
    int LightsBridge::LightType() const {
        const auto* L = SelOf(Renderer);
        return (L && L->Type == Smile::ELightType::Spot) ? 1 : 0;
    }
    bool LightsBridge::LightEnabled() const {
        const auto* L = SelOf(Renderer);
        return L ? L->Enabled : false;
    }
    bool LightsBridge::CastShadows() const {
        const auto* L = SelOf(Renderer);
        return L ? L->CastShadows : true;
    }
    QColor LightsBridge::Color() const {
        const auto* L = SelOf(Renderer);
        if (!L) return QColor(255, 255, 255);
        return QColor::fromRgbF(std::clamp(L->Color.X, 0.0f, 1.0f),
                                std::clamp(L->Color.Y, 0.0f, 1.0f),
                                std::clamp(L->Color.Z, 0.0f, 1.0f));
    }
    double LightsBridge::Intensity() const    { const auto* L = SelOf(Renderer); return L ? L->Intensity : 0.0; }
    double LightsBridge::Radius() const       { const auto* L = SelOf(Renderer); return L ? L->AttenuationRadius : 0.0; }
    double LightsBridge::SourceRadius() const { const auto* L = SelOf(Renderer); return L ? L->SourceRadius : 0.05; }
    double LightsBridge::InnerConeDeg() const { const auto* L = SelOf(Renderer); return L ? L->InnerConeDeg : 25.0; }
    double LightsBridge::OuterConeDeg() const { const auto* L = SelOf(Renderer); return L ? L->OuterConeDeg : 40.0; }
    double LightsBridge::PosX() const         { const auto* L = SelOf(Renderer); return L ? L->Position.X : 0.0; }
    double LightsBridge::PosY() const         { const auto* L = SelOf(Renderer); return L ? L->Position.Y : 0.0; }
    double LightsBridge::PosZ() const         { const auto* L = SelOf(Renderer); return L ? L->Position.Z : 0.0; }

    double LightsBridge::SpotAzimuthDeg() const {
        const auto* L = SelOf(Renderer);
        if (!L) return 0.0;
        double Az = std::atan2((double)L->Direction.X, (double)L->Direction.Z) * kToDeg;
        return Az < 0.0 ? Az + 360.0 : Az;
    }
    double LightsBridge::SpotElevationDeg() const {
        const auto* L = SelOf(Renderer);
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

    void LightsBridge::SetName(const QString& _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        const std::string S = _V.trimmed().toStdString();
        if (S.empty() || L->Name == S) return;
        L->Name = S;
        Touch(true); // nome aparece na lista
    }
    void LightsBridge::SetLightEnabled(bool _V) {
        auto* L = SelOf(Renderer); if (!L || L->Enabled == _V) return;
        L->Enabled = _V;
        Touch(true); // estado aparece na lista
    }
    void LightsBridge::SetCastShadows(bool _V) {
        auto* L = SelOf(Renderer); if (!L || L->CastShadows == _V) return;
        L->CastShadows = _V;
        Touch(false);
    }
    void LightsBridge::SetColor(const QColor& _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->Color = { (float)_V.redF(), (float)_V.greenF(), (float)_V.blueF() };
        Touch(true); // dot de cor na lista
    }
    void LightsBridge::SetIntensity(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->Intensity = (float)std::max(_V, 0.0);
        Touch(false);
    }
    void LightsBridge::SetRadius(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->AttenuationRadius = (float)std::clamp(_V, 0.1, 500.0);
        Touch(false);
    }
    void LightsBridge::SetSourceRadius(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->SourceRadius = (float)std::clamp(_V, 0.01, 2.0);
        Touch(false);
    }
    void LightsBridge::SetInnerConeDeg(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->InnerConeDeg = (float)std::clamp(_V, 0.0, 89.0);
        L->OuterConeDeg = std::max(L->OuterConeDeg, L->InnerConeDeg); // inner <= outer sempre
        Touch(false);
    }
    void LightsBridge::SetOuterConeDeg(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->OuterConeDeg = (float)std::clamp(_V, 1.0, 89.0);
        L->InnerConeDeg = std::min(L->InnerConeDeg, L->OuterConeDeg);
        Touch(false);
    }
    void LightsBridge::SetPosX(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->Position.X = (float)_V; Touch(false);
    }
    void LightsBridge::SetPosY(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->Position.Y = (float)_V; Touch(false);
    }
    void LightsBridge::SetPosZ(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->Position.Z = (float)_V; Touch(false);
    }
    void LightsBridge::SetSpotAzimuthDeg(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->Direction = DirFromAzEl(_V, SpotElevationDeg());
        Touch(false);
    }
    void LightsBridge::SetSpotElevationDeg(double _V) {
        auto* L = SelOf(Renderer); if (!L) return;
        L->Direction = DirFromAzEl(SpotAzimuthDeg(), std::clamp(_V, -90.0, 90.0));
        Touch(false);
    }

    // ---- Lista ----
    QVariantMap LightsBridge::lightAt(int _Index) const {
        QVariantMap M;
        const auto& L = LightsOf(Renderer);
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

        auto& Lights = Renderer->GetScene().Lights();
        Lights.push_back(L);
        Renderer->SetSelectedLight((int)Lights.size() - 1);
        Renderer->ClearSelection(); // selecao de luz e de renderavel sao exclusivas
        Touch(true);
        emit SelectionChanged();
    }

    void LightsBridge::removeLight(int _Index) {
        if (!Renderer) return;
        auto& Lights = Renderer->GetScene().Lights();
        if (_Index < 0 || _Index >= (int)Lights.size()) return;
        Lights.erase(Lights.begin() + _Index);

        int Sel = Renderer->GetSelectedLight();
        if (Sel == _Index)     Sel = -1;
        else if (Sel > _Index) --Sel;
        Renderer->SetSelectedLight(Sel);

        Touch(true);
        emit SelectionChanged();
    }

    void LightsBridge::duplicateLight(int _Index) {
        if (!Renderer) return;
        auto& Lights = Renderer->GetScene().Lights();
        if (_Index < 0 || _Index >= (int)Lights.size()) return;
        Smile::FLight Copy = Lights[(size_t)_Index];
        Copy.Id = 0; // copia nasce SEM identidade: o renderer atribui uma nova no proximo
                     // frame. Herdar o Id faria as duas luzes disputarem o mesmo slot de
                     // shadow map (uma piscaria em cima da outra).
        Copy.Name += " (copia)";
        Copy.Position.X += 1.0f; // desloca pro marker nao nascer em cima do original
        Lights.push_back(Copy);
        Renderer->SetSelectedLight((int)Lights.size() - 1);
        Renderer->ClearSelection();
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
        auto& Lights = Renderer->GetScene().Lights();
        if (_Index < 0 || _Index >= (int)Lights.size()) return;
        Lights[(size_t)_Index].Enabled = !Lights[(size_t)_Index].Enabled;
        Touch(true);
    }

    void LightsBridge::placeAtCamera() {
        auto* L = SelOf(Renderer); if (!L) return;
        const double Pitch = Renderer->GetPitch() * kToRad;
        const double Yaw   = Renderer->GetYaw()   * kToRad;
        const Smile::Vec3 Fwd = { (float)(std::cos(Pitch) * std::sin(Yaw)),
                                  (float)std::sin(Pitch),
                                  (float)(std::cos(Pitch) * std::cos(Yaw)) };
        L->Position = Renderer->GetCameraPos() + Fwd * 5.0f;
        if (L->Type == Smile::ELightType::Spot) L->Direction = Fwd; // spot aponta pro olhar
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

        auto& Lights = Renderer->GetScene().Lights();
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
            Lights.push_back(L);
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
            Arr.append(O);
        }
        QJsonObject Root;
        Root[QStringLiteral("version")] = 1;
        Root[QStringLiteral("lights")]  = Arr;

        QFile File(JsonPath);
        if (!File.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            Smile::LogError("Luzes: falha ao salvar " + JsonPath.toStdString());
            return false;
        }
        File.write(QJsonDocument(Root).toJson(QJsonDocument::Indented));
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
            const auto* L = SelOf(Renderer);
            if (L) {
                CachedPos[0] = L->Position.X;
                CachedPos[1] = L->Position.Y;
                CachedPos[2] = L->Position.Z;
            }
            emit SelectionChanged();
            emit LightChanged();
            return;
        }

        // Gizmo arrastando a luz: reflete a posicao no painel e marca dirty.
        const auto* L = SelOf(Renderer);
        if (L && (std::abs(L->Position.X - CachedPos[0]) > 1e-5 ||
                  std::abs(L->Position.Y - CachedPos[1]) > 1e-5 ||
                  std::abs(L->Position.Z - CachedPos[2]) > 1e-5)) {
            CachedPos[0] = L->Position.X;
            CachedPos[1] = L->Position.Y;
            CachedPos[2] = L->Position.Z;
            if (!DirtyFlag) { DirtyFlag = true; emit DirtyChanged(); }
            emit LightChanged();
        }
    }
}
