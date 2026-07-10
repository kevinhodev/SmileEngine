#include "SmileEditor/GizmoController.h"
#include "Smile/Graphics/Renderer.h"
#include "Smile/Scene/Scene.h"
#include <algorithm>
#include <cmath>

namespace SmileEditor {
    using Smile::Vec3;
    using Smile::u32;

    namespace {
        Vec3 AxisDir(GizmoController::EAxis A) {
            return A == GizmoController::EAxis::X ? Vec3::UnitX()
                 : A == GizmoController::EAxis::Y ? Vec3::UnitY() : Vec3::UnitZ();
        }
        constexpr float kFovY      = 60.0f * 3.14159265f / 180.0f; // bate com a Projection da camera
        constexpr float kSizeFrac  = 0.18f;  // tamanho da seta ~18% da meia-altura da tela
        constexpr float kHitRadius = 9.0f;   // pixels
        constexpr float kPi        = 3.14159265f;

        // Circulo wireframe generico (centro, dois eixos do plano, raio) no DebugDraw.
        void DrawCircle(Smile::FDebugDraw& DD, const Vec3& C, const Vec3& U, const Vec3& V,
                        float R, const Vec3& Col, int Segs = 40) {
            Vec3 Prev = C + U * R;
            for (int s = 1; s <= Segs; ++s) {
                const float a = (2.0f * kPi * s) / Segs;
                const Vec3 P = C + (U * std::cos(a) + V * std::sin(a)) * R;
                DD.Line(Prev, P, Col);
                Prev = P;
            }
        }
    }

    bool GizmoController::GetPivot(Smile::Renderer& R, Vec3& OutPivot, int& OutIdx,
                                   bool& OutIsLight) const {
        // Luz selecionada tem prioridade (o editor mantem as duas selecoes exclusivas).
        const int LightIdx = R.GetSelectedLight();
        const auto& Lights = R.GetScene().Lights();
        if (LightIdx >= 0 && LightIdx < static_cast<int>(Lights.size())) {
            OutIdx     = LightIdx;
            OutIsLight = true;
            OutPivot   = Lights[static_cast<size_t>(LightIdx)].Position;
            return true;
        }

        OutIsLight = false;
        OutIdx = R.GetSelectedObject();
        if (OutIdx < 0) return false;
        const auto& List = R.GetScene().Renderables();
        if (OutIdx >= static_cast<int>(List.size())) { OutIdx = -1; return false; }
        const Smile::FRenderable& Rn = List[static_cast<size_t>(OutIdx)];
        OutPivot = (Rn.AABBMin + Rn.AABBMax) * 0.5f;
        return true;
    }

    float GizmoController::ScaleFor(Smile::Renderer& R, const Vec3& Pivot) const {
        const float Dist = (Pivot - R.GetCameraPos()).Length();
        return std::max(0.001f, Dist * std::tan(kFovY * 0.5f) * kSizeFrac);
    }

    GizmoController::EAxis GizmoController::HitTest(Smile::Renderer& _Renderer, u32 _X, u32 _Y) const {
        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(_Renderer, Pivot, Idx, IsLight)) return EAxis::None;
        const float Scale = ScaleFor(_Renderer, Pivot);
        float px0, py0;
        if (!_Renderer.WorldToScreen(Pivot, px0, py0)) return EAxis::None;

        const EAxis Ids[3] = { EAxis::X, EAxis::Y, EAxis::Z };
        const float fx = static_cast<float>(_X), fy = static_cast<float>(_Y);
        float Best = kHitRadius; EAxis BestAxis = EAxis::None;
        for (int i = 0; i < 3; ++i) {
            float px1, py1;
            if (!_Renderer.WorldToScreen(Pivot + AxisDir(Ids[i]) * Scale, px1, py1)) continue;
            const float dx = px1 - px0, dy = py1 - py0;
            const float len2 = dx*dx + dy*dy;
            float t = (len2 > 1e-6f) ? (((fx - px0)*dx + (fy - py0)*dy) / len2) : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float cxp = px0 + dx*t, cyp = py0 + dy*t;
            const float dist = std::sqrt((fx - cxp)*(fx - cxp) + (fy - cyp)*(fy - cyp));
            if (dist < Best) { Best = dist; BestAxis = Ids[i]; }
        }
        return BestAxis;
    }

    float GizmoController::AxisParam(const Vec3& O, const Vec3& Dir, const Vec3& A, const Vec3& Pivot) const {
        const Vec3 w0 = Pivot - O;
        const float b = A.Dot(Dir);
        const float d = A.Dot(w0);
        const float e = Dir.Dot(w0);
        const float denom = 1.0f - b*b; // a=c=1 (unitarios)
        if (std::fabs(denom) < 1e-5f) return DragStartT; // raio ~paralelo ao eixo: nao move
        return (b*e - d) / denom;
    }

    void GizmoController::SubmitLightShapes(Smile::Renderer& R) const {
        const auto& Lights = R.GetScene().Lights();
        if (Lights.empty()) return;

        Smile::FDebugDraw& DD = R.GetDebugDraw();
        const int Selected = R.GetSelectedLight();

        for (int i = 0; i < static_cast<int>(Lights.size()); ++i) {
            const Smile::FLight& L = Lights[static_cast<size_t>(i)];

            // Marker (estrela 3D pequena, constante em tela) em TODAS as luzes — cor da propria
            // luz; apagada fica cinza. E o alvo do clique de selecao no viewport.
            const float S = ScaleFor(R, L.Position) * 0.10f;
            const Vec3 MCol = L.Enabled
                ? Vec3{ std::max(L.Color.X, 0.15f), std::max(L.Color.Y, 0.15f),
                        std::max(L.Color.Z, 0.15f) }
                : Vec3{ 0.35f, 0.35f, 0.35f };
            const Vec3 P = L.Position;
            DD.Line(P - Vec3::UnitX() * S, P + Vec3::UnitX() * S, MCol);
            DD.Line(P - Vec3::UnitY() * S, P + Vec3::UnitY() * S, MCol);
            DD.Line(P - Vec3::UnitZ() * S, P + Vec3::UnitZ() * S, MCol);
            // diagonais curtas dao leitura de "estrela"/bulbo
            const float D2 = S * 0.55f;
            DD.Line(P - Vec3{ D2, D2, 0.0f }, P + Vec3{ D2, D2, 0.0f }, MCol);
            DD.Line(P - Vec3{ 0.0f, D2, D2 }, P + Vec3{ 0.0f, D2, D2 }, MCol);

            if (i != Selected) continue;

            // Selecionada: a forma do volume de influencia, na cor da luz.
            if (L.Type == Smile::ELightType::Point) {
                // 3 grandes circulos (XY/XZ/YZ) no raio de atenuacao.
                DrawCircle(DD, P, Vec3::UnitX(), Vec3::UnitY(), L.AttenuationRadius, MCol);
                DrawCircle(DD, P, Vec3::UnitX(), Vec3::UnitZ(), L.AttenuationRadius, MCol);
                DrawCircle(DD, P, Vec3::UnitY(), Vec3::UnitZ(), L.AttenuationRadius, MCol);
            } else {
                // Spot: cone INSCRITO na esfera de atenuacao (estilo DrawWireSphereCappedCone
                // da UE). A luz morre a Range da POSICAO (janela esferica (1-(d/r)^4)^2), entao
                // as arestas tem comprimento Range e a tampa fica em Range*cos(theta) com raio
                // Range*sin(theta) — tampa a Range AO LONGO DO EIXO com raio tan() desenhava
                // cantos onde a luz e zero. Calota esferica em 2 planos fecha a leitura.
                const Vec3 Dir = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                const Vec3 U = Dir.GetOrthogonal();
                const Vec3 V = Dir.Cross(U).Normalized();
                const float Range    = L.AttenuationRadius;
                const float OuterRad = std::clamp(L.OuterConeDeg, 1.0f, 89.0f) * kPi / 180.0f;
                const float InnerRad = std::clamp(L.InnerConeDeg, 0.0f, L.OuterConeDeg) * kPi / 180.0f;

                const Vec3  CapC = P + Dir * (Range * std::cos(OuterRad));
                const float CapR = Range * std::sin(OuterRad);
                DrawCircle(DD, CapC, U, V, CapR, MCol, 32);
                for (int e = 0; e < 4; ++e) {
                    const float a = (2.0f * kPi * e) / 4.0f;
                    const Vec3 Rim = CapC + (U * std::cos(a) + V * std::sin(a)) * CapR;
                    DD.Line(P, Rim, MCol);
                }
                // Calota esferica: arcos de -theta..+theta nos planos Dir/U e Dir/V.
                const Vec3 Planes[2] = { U, V };
                for (const Vec3& Pl : Planes) {
                    Vec3 Prev = P + (Dir * std::cos(-OuterRad) + Pl * std::sin(-OuterRad)) * Range;
                    constexpr int kArcSegs = 16;
                    for (int s = 1; s <= kArcSegs; ++s) {
                        const float t = -OuterRad + (2.0f * OuterRad * s) / kArcSegs;
                        const Vec3 Pt = P + (Dir * std::cos(t) + Pl * std::sin(t)) * Range;
                        DD.Line(Prev, Pt, MCol);
                        Prev = Pt;
                    }
                }
                // Cone interno (onde o falloff angular comeca), mais fraco.
                const Vec3 DimCol = MCol * 0.45f;
                DrawCircle(DD, P + Dir * (Range * std::cos(InnerRad)), U, V,
                           Range * std::sin(InnerRad), DimCol, 32);
            }
        }
    }

    void GizmoController::Submit(Smile::Renderer& _Renderer) {
        if (!Enabled) return;

        // Visualizacao das luzes independe de haver selecao (markers sempre visiveis).
        SubmitLightShapes(_Renderer);

        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(_Renderer, Pivot, Idx, IsLight)) return;
        const float Scale = ScaleFor(_Renderer, Pivot);

        Smile::FDebugDraw& DebugDraw = _Renderer.GetDebugDraw();
        const Vec3 Axis[3]    = { Vec3::UnitX(), Vec3::UnitY(), Vec3::UnitZ() };
        const Vec3 BaseCol[3] = { {1.0f,0.25f,0.25f}, {0.30f,1.0f,0.30f}, {0.35f,0.55f,1.0f} };
        const EAxis Ids[3]    = { EAxis::X, EAxis::Y, EAxis::Z };
        const Vec3 Hi = { 1.0f, 1.0f, 0.15f };
        const EAxis Highlighted = Dragging ? Active : Hovered;

        const float Len = Scale, HeadLen = Scale*0.22f, HeadR = Scale*0.075f, ShaftEnd = Len - HeadLen;
        for (int i = 0; i < 3; ++i) {
            const Vec3 d   = Axis[i];
            const Vec3 col = (Highlighted == Ids[i]) ? Hi : BaseCol[i];
            DebugDraw.Line(Pivot, Pivot + d * ShaftEnd, col);
            // Ponta (piramide de 4 faces).
            const Vec3 u = (i == 0) ? Vec3::UnitY() : Vec3::UnitX();
            const Vec3 v = (i == 2) ? Vec3::UnitY() : Vec3::UnitZ();
            const Vec3 tip = Pivot + d * Len;
            const Vec3 bc  = Pivot + d * ShaftEnd;
            const Vec3 c[4] = { bc + u*HeadR + v*HeadR, bc - u*HeadR + v*HeadR,
                                bc - u*HeadR - v*HeadR, bc + u*HeadR - v*HeadR };
            for (int s = 0; s < 4; ++s)
                DebugDraw.Triangle(tip, c[s], c[(s + 1) & 3], col);
        }
    }

    bool GizmoController::OnMousePress(Smile::Renderer& R, u32 X, u32 Y) {
        if (!Enabled) return false;
        const EAxis Axis = HitTest(R, X, Y);
        if (Axis == EAxis::None) return false;
        Vec3 O, Dir;
        if (!R.ScreenToRay(X, Y, O, Dir)) return false;
        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(R, Pivot, Idx, IsLight)) return false;

        Active         = Axis;
        Hovered        = Axis;
        Dragging       = true;
        DragIdx        = Idx;
        DragIsLight    = IsLight;
        DragStartPivot = Pivot;
        DragStartPos   = IsLight
            ? R.GetScene().Lights()[static_cast<size_t>(Idx)].Position
            : R.GetScene().Renderables()[static_cast<size_t>(Idx)].Transform.Position;
        DragStartT     = AxisParam(O, Dir, AxisDir(Axis), Pivot);
        return true;
    }

    void GizmoController::OnMouseMove(Smile::Renderer& R, u32 X, u32 Y) {
        if (!Enabled) return;
        if (!Dragging) { Hovered = HitTest(R, X, Y); return; }
        if (DragIdx < 0) return;
        Vec3 O, Dir;
        if (!R.ScreenToRay(X, Y, O, Dir)) return;
        const Vec3 A = AxisDir(Active);
        const float T = AxisParam(O, Dir, A, DragStartPivot);
        const Vec3 NewPos = DragStartPos + A * (T - DragStartT);

        if (DragIsLight) {
            auto& Lights = R.GetScene().Lights();
            if (DragIdx >= static_cast<int>(Lights.size())) return;
            Lights[static_cast<size_t>(DragIdx)].Position = NewPos;
            return;
        }

        auto& List = R.GetScene().Renderables();
        if (DragIdx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(DragIdx)];
        const Vec3 Step = NewPos - Rn.Transform.Position; // incremento deste passo
        Rn.Transform.Position = NewPos;
        Rn.AABBMin += Step; // a AABB de mundo acompanha (frustum culling)
        Rn.AABBMax += Step;
    }

    void GizmoController::OnMouseRelease() {
        Dragging    = false;
        Active      = EAxis::None;
        DragIdx     = -1;
        DragIsLight = false;
    }
}
