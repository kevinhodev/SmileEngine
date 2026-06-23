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
    }

    bool GizmoController::GetPivot(Smile::Renderer& R, Vec3& OutPivot, int& OutIdx) const {
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
        Vec3 Pivot; int Idx;
        if (!GetPivot(_Renderer, Pivot, Idx)) return EAxis::None;
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

    void GizmoController::Submit(Smile::Renderer& _Renderer) {
        if (!Enabled) return;
        Vec3 Pivot; int Idx;
        if (!GetPivot(_Renderer, Pivot, Idx)) return;
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
        Vec3 Pivot; int Idx;
        if (!GetPivot(R, Pivot, Idx)) return false;

        Active         = Axis;
        Hovered        = Axis;
        Dragging       = true;
        DragIdx        = Idx;
        DragStartPivot = Pivot;
        DragStartPos   = R.GetScene().Renderables()[static_cast<size_t>(Idx)].Transform.Position;
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

        auto& List = R.GetScene().Renderables();
        if (DragIdx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(DragIdx)];
        const Vec3 Step = NewPos - Rn.Transform.Position; // incremento deste passo
        Rn.Transform.Position = NewPos;
        Rn.AABBMin += Step; // a AABB de mundo acompanha (frustum culling)
        Rn.AABBMax += Step;
    }

    void GizmoController::OnMouseRelease() {
        Dragging = false;
        Active   = EAxis::None;
        DragIdx  = -1;
    }
}
