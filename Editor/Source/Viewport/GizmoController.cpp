#include "SmileEditor/Viewport/GizmoController.h"
#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Scene/Scene.h"
#include <algorithm>
#include <cmath>

namespace SmileEditor {
    using Smile::Vec2;
    using Smile::Vec3;
    using Smile::Mat44;
    using Smile::u32;

    namespace {
        constexpr float kSizeFrac  = 0.18f;
        constexpr float kHitRadius = 9.0f;
        constexpr float kIconFrac  = 0.22f;
        constexpr float kAxisThickness = 3.0f;
        constexpr float kSpotThickness = 2.0f;
        constexpr float kRingThickness = 2.5f;
        constexpr float kRingRadiusFrac = 0.85f;
        constexpr int   kRingSegments   = 64;
        // Mantém transforms invertíveis; o gizmo não representa espelhamento.
        constexpr float kMinScale = 0.001f;

        Vec3 AxisFromEnum(const Vec3 Axes[3], GizmoController::EAxis A) {
            return A == GizmoController::EAxis::X ? Axes[0]
                 : A == GizmoController::EAxis::Y ? Axes[1] : Axes[2];
        }
        int IndexFromEnum(GizmoController::EAxis A) {
            return A == GizmoController::EAxis::X ? 0
                 : A == GizmoController::EAxis::Y ? 1 : 2;
        }
        // TVec3 possui membros nomeados; indexação por aritmética de ponteiros seria UB.
        float& Comp(Vec3& V, int I) { return I == 0 ? V.X : (I == 1 ? V.Y : V.Z); }
        float  Comp(const Vec3& V, int I) { return I == 0 ? V.X : (I == 1 ? V.Y : V.Z); }

    }

    float GizmoController::IconHalfSizeFor(Smile::Renderer& R, const Vec3& Position) const {
        return ScaleFor(R, Position) * kIconFrac;
    }

    int GizmoController::PickLightIcon(Smile::Renderer& R, u32 _X, u32 _Y) const {
        // Um ícone invisível não deve interceptar o picking de objetos.
        if (!Enabled) return -1;
        const auto& Lights = R.GetScene().Lights();
        if (Lights.empty()) return -1;

        const Vec3  CamRight = R.GetCameraRight();
        const float fx = static_cast<float>(_X), fy = static_cast<float>(_Y);

        int BestIdx = -1;
        for (int i = 0; i < static_cast<int>(Lights.size()); ++i) {
            const Vec3& P = Lights[static_cast<size_t>(i)].Position;
            float cx, cy, ex, ey;
            if (!R.WorldToScreen(P, cx, cy)) continue;
            if (!R.WorldToScreen(P + CamRight * IconHalfSizeFor(R, P), ex, ey)) continue;

            const float RadiusPx2 = (ex - cx) * (ex - cx) + (ey - cy) * (ey - cy);
            const float dx = fx - cx, dy = fy - cy;
            if (RadiusPx2 <= 0.0f || dx * dx + dy * dy > RadiusPx2) continue;

            // Em sobreposição, vence o último ícone desenhado.
            BestIdx = i;
        }
        return BestIdx;
    }

    bool GizmoController::GetPivot(Smile::Renderer& R, Vec3& OutPivot, int& OutIdx,
                                   bool& OutIsLight) const {
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
        return std::max(0.001f, Dist * std::tan(R.GetFovY() * 0.5f) * kSizeFrac);
    }

    void GizmoController::AxisBasis(Smile::Renderer& R, bool IsLight, int Idx,
                                    Vec3 Out[3]) const {
        if (Dragging) { for (int i = 0; i < 3; ++i) Out[i] = DragAxes[i]; return; }

        Out[0] = Vec3::UnitX(); Out[1] = Vec3::UnitY(); Out[2] = Vec3::UnitZ();
        // Eventos usam a seleção atual, que pode ser mais nova que o cache da UI.
        if (SpaceFor(IsLight) == ESpace::World || Idx < 0) return;
        const auto& List = R.GetScene().Renderables();
        if (Idx >= static_cast<int>(List.size())) return;
        const Mat44 Rot = Mat44::RotationEulerXYZ(
            List[static_cast<size_t>(Idx)].Transform.RotationEuler);
        for (int i = 0; i < 3; ++i) Out[i] = Rot.GetRow3(i).NormalizedSafe(Out[i]);
    }

    GizmoController::EAxis GizmoController::HitTestAxes(Smile::Renderer& _Renderer,
                                                        u32 _X, u32 _Y) const {
        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(_Renderer, Pivot, Idx, IsLight)) return EAxis::None;
        if (Mode == EMode::Scale && IsLight) return EAxis::None;
        const float Scale = ScaleFor(_Renderer, Pivot);
        float px0, py0;
        if (!_Renderer.WorldToScreen(Pivot, px0, py0)) return EAxis::None;

        Vec3 Axes[3];
        AxisBasis(_Renderer, IsLight, Idx, Axes);
        const EAxis Ids[3] = { EAxis::X, EAxis::Y, EAxis::Z };
        const float fx = static_cast<float>(_X), fy = static_cast<float>(_Y);
        const Vec2 Mouse{ fx, fy };
        float Best = kHitRadius; EAxis BestAxis = EAxis::None;
        for (int i = 0; i < 3; ++i) {
            float px1, py1;
            if (!_Renderer.WorldToScreen(Pivot + Axes[i] * Scale, px1, py1)) continue;
            const Vec2 Closest = Smile::ClosestPointOnSegment(
                Mouse, Vec2{ px0, py0 }, Vec2{ px1, py1 });
            const float dist = (Mouse - Closest).Length();
            if (dist < Best) { Best = dist; BestAxis = Ids[i]; }
        }
        return BestAxis;
    }

    GizmoController::EAxis GizmoController::HitTestRings(Smile::Renderer& _Renderer,
                                                         u32 _X, u32 _Y,
                                                         Vec3* OutRingPoint) const {
        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(_Renderer, Pivot, Idx, IsLight)) return EAxis::None;
        if (IsLight) {
            const auto& Lights = _Renderer.GetScene().Lights();
            if (Idx >= static_cast<int>(Lights.size())) return EAxis::None;
            if (Lights[static_cast<size_t>(Idx)].Type != Smile::ELightType::Spot)
                return EAxis::None;
        }

        const float Radius = ScaleFor(_Renderer, Pivot) * kRingRadiusFrac;
        Vec3 Axes[3];
        AxisBasis(_Renderer, IsLight, Idx, Axes);
        const Vec3  CamDir = _Renderer.GetCameraPos() - Pivot;
        const EAxis Ids[3] = { EAxis::X, EAxis::Y, EAxis::Z };
        const float fx = static_cast<float>(_X), fy = static_cast<float>(_Y);
        const Vec2 Mouse{ fx, fy };

        float Best = kHitRadius; EAxis BestAxis = EAxis::None;
        for (int i = 0; i < 3; ++i) {
            const Vec3 U = Axes[(i + 1) % 3], V = Axes[(i + 2) % 3];
            float PrevX = 0.0f, PrevY = 0.0f;
            Vec3 PrevW{};
            bool PrevOk = false, PrevNear = false;
            for (int s = 0; s <= kRingSegments; ++s) {
                const float  a = Smile::TwoPi * static_cast<float>(s) / kRingSegments;
                const Vec3   Offset = U * std::cos(a) + V * std::sin(a);
                const Vec3   W  = Pivot + Offset * Radius;
                float cx = 0.0f, cy = 0.0f;
                const bool Ok = _Renderer.WorldToScreen(W, cx, cy);
                const bool Near = Offset.Dot(CamDir) > 0.0f;
                if (Ok && PrevOk && Near && PrevNear) {
                    float SegmentT = 0.0f;
                    const Vec2 Closest = Smile::ClosestPointOnSegment(
                        Mouse, Vec2{ PrevX, PrevY }, Vec2{ cx, cy }, &SegmentT);
                    const float dist = (Mouse - Closest).Length();
                    if (dist < Best) {
                        Best = dist; BestAxis = Ids[i];
                        if (OutRingPoint) *OutRingPoint = PrevW + (W - PrevW) * SegmentT;
                    }
                }
                if (Ok) { PrevX = cx; PrevY = cy; PrevW = W; }
                PrevOk = Ok; PrevNear = Near;
            }
        }
        return BestAxis;
    }

    GizmoController::EAxis GizmoController::HitTest(Smile::Renderer& R, u32 X, u32 Y) const {
        switch (Mode) {
            case EMode::Translate:
            case EMode::Scale:  return HitTestAxes(R, X, Y);
            case EMode::Rotate: return HitTestRings(R, X, Y, nullptr);
            default:            return EAxis::None;
        }
    }

    float GizmoController::AxisParam(const Vec3& O, const Vec3& Dir, const Vec3& A, const Vec3& Pivot) const {
        const Vec3 w0 = Pivot - O;
        const float b = A.Dot(Dir);
        const float d = A.Dot(w0);
        const float e = Dir.Dot(w0);
        const float denom = 1.0f - b*b;
        if (std::fabs(denom) < 1e-5f) return DragStartT;
        return (b*e - d) / denom;
    }

    void GizmoController::SubmitLightShapes(Smile::Renderer& R) const {
        const auto& Lights = R.GetScene().Lights();
        if (Lights.empty()) return;

        Smile::FDebugDraw& DD = R.GetDebugDraw();
        const int Selected = R.GetSelectedLight();

        for (int i = 0; i < static_cast<int>(Lights.size()); ++i) {
            const Smile::FLight& L = Lights[static_cast<size_t>(i)];

            const float S = IconHalfSizeFor(R, L.Position);
            const Vec3 MCol = L.Enabled
                ? Vec3{ std::max(L.Color.X, 0.15f), std::max(L.Color.Y, 0.15f),
                        std::max(L.Color.Z, 0.15f) }
                : Vec3{ 0.35f, 0.35f, 0.35f };
            const bool IsSel = (i == Selected);
            DD.Icon(L.Position, S, MCol,
                    L.Type == Smile::ELightType::Spot ? 1u : 0u, IsSel);

            if (IsSel && L.Type == Smile::ELightType::Spot) {
                const Vec3 Dir = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                DD.Line(L.Position + Dir * (S * 1.4f), L.Position + Dir * (S * 4.0f),
                        Smile::Vec4{ MCol, 1.0f },
                        Smile::EDebugDepthMode::Foreground, kSpotThickness);
            }
        }
    }

    namespace {
        const Vec3 kAxisColor[3] = { {1.0f,0.25f,0.25f}, {0.30f,1.0f,0.30f}, {0.35f,0.55f,1.0f} };
        const Vec3 kHighlight    = { 1.0f, 1.0f, 0.15f };
    }

    void GizmoController::SubmitTranslate(Smile::Renderer& _Renderer, const Vec3& Pivot,
                                          float Scale, const Vec3 Axes[3]) const {
        Smile::FDebugDraw& DD = _Renderer.GetDebugDraw();
        const EAxis Ids[3] = { EAxis::X, EAxis::Y, EAxis::Z };
        const EAxis Highlighted = Dragging ? Active : Hovered;
        const float Len = Scale, HeadLen = Scale*0.22f, HeadR = Scale*0.075f;
        const float ShaftEnd = Len - HeadLen;

        for (int i = 0; i < 3; ++i) {
            const Vec3 d   = Axes[i];
            const Vec3 col = (Highlighted == Ids[i]) ? kHighlight : kAxisColor[i];
            DD.Line(Pivot, Pivot + d * ShaftEnd, Smile::Vec4{ col, 1.0f },
                    Smile::EDebugDepthMode::Foreground, kAxisThickness);
            const Vec3 u = Axes[(i + 1) % 3], v = Axes[(i + 2) % 3];
            const Vec3 tip = Pivot + d * Len;
            const Vec3 bc  = Pivot + d * ShaftEnd;
            const Vec3 c[4] = { bc + u*HeadR + v*HeadR, bc - u*HeadR + v*HeadR,
                                bc - u*HeadR - v*HeadR, bc + u*HeadR - v*HeadR };
            for (int s = 0; s < 4; ++s)
                DD.Triangle(tip, c[s], c[(s + 1) & 3], Smile::Vec4{ col, 1.0f });
        }
    }

    void GizmoController::SubmitRotate(Smile::Renderer& _Renderer, const Vec3& Pivot,
                                       float Scale, const Vec3 Axes[3]) const {
        Smile::FDebugDraw& DD = _Renderer.GetDebugDraw();
        const EAxis Ids[3] = { EAxis::X, EAxis::Y, EAxis::Z };
        const EAxis Highlighted = Dragging ? Active : Hovered;
        const float Radius = Scale * kRingRadiusFrac;
        const Vec3  CamDir = _Renderer.GetCameraPos() - Pivot;

        for (int i = 0; i < 3; ++i) {
            if (Dragging && Highlighted != Ids[i]) continue;
            const Vec3  col = (Highlighted == Ids[i]) ? kHighlight : kAxisColor[i];
            const Vec3  U = Axes[(i + 1) % 3], V = Axes[(i + 2) % 3];
            Vec3 Prev = Pivot + U * Radius;
            bool PrevNear = U.Dot(CamDir) > 0.0f;
            for (int s = 1; s <= kRingSegments; ++s) {
                const float a = Smile::TwoPi * static_cast<float>(s) / kRingSegments;
                const Vec3  Offset = U * std::cos(a) + V * std::sin(a);
                const Vec3  W = Pivot + Offset * Radius;
                const bool  Near = Offset.Dot(CamDir) > 0.0f;
                // A metade traseira permanece translúcida para indicar o plano do anel.
                const bool  Solid = Near && PrevNear;
                DD.Line(Prev, W, Smile::Vec4{ col, Solid ? 1.0f : 0.22f },
                        Smile::EDebugDepthMode::Foreground,
                        Solid ? kRingThickness : 1.5f);
                Prev = W; PrevNear = Near;
            }
        }

        // Mostra os raios inicial e atual do gesto.
        if (Dragging && Active != EAxis::None) {
            const Vec3 Start = DragStartRingDir * Radius;
            const Vec3 Cur = Mat44::RotationAxis(DragAxisWorld, DragAngle)
                                 .TransformVectorRow(Start);
            DD.Line(Pivot, Pivot + Start, Smile::Vec4{ Vec3{0.85f,0.85f,0.85f}, 0.65f },
                    Smile::EDebugDepthMode::Foreground, 1.5f);
            DD.Line(Pivot, Pivot + Cur, Smile::Vec4{ kHighlight, 1.0f },
                    Smile::EDebugDepthMode::Foreground, 2.0f);
        }
    }

    void GizmoController::SubmitScale(Smile::Renderer& _Renderer, const Vec3& Pivot,
                                      float Scale, const Vec3 Axes[3]) const {
        Smile::FDebugDraw& DD = _Renderer.GetDebugDraw();
        const EAxis Ids[3] = { EAxis::X, EAxis::Y, EAxis::Z };
        const EAxis Highlighted = Dragging ? Active : Hovered;
        const float Len = Scale, BoxR = Scale * 0.075f;
        const float ShaftEnd = Len - BoxR;
        static const int kFace[6][4] = { {0,2,6,4}, {1,5,7,3}, {0,4,5,1},
                                         {2,3,7,6}, {0,1,3,2}, {4,6,7,5} };

        for (int i = 0; i < 3; ++i) {
            const Vec3 d   = Axes[i];
            const Vec3 col = (Highlighted == Ids[i]) ? kHighlight : kAxisColor[i];
            DD.Line(Pivot, Pivot + d * ShaftEnd, Smile::Vec4{ col, 1.0f },
                    Smile::EDebugDepthMode::Foreground, kAxisThickness);

            const Vec3 c  = Pivot + d * Len;
            const Vec3 e0 = d * BoxR, e1 = Axes[(i + 1) % 3] * BoxR, e2 = Axes[(i + 2) % 3] * BoxR;
            Vec3 Corner[8];
            for (int k = 0; k < 8; ++k)
                Corner[k] = c + e0 * ((k & 1) ? 1.0f : -1.0f)
                              + e1 * ((k & 2) ? 1.0f : -1.0f)
                              + e2 * ((k & 4) ? 1.0f : -1.0f);
            for (int f = 0; f < 6; ++f) {
                DD.Triangle(Corner[kFace[f][0]], Corner[kFace[f][1]], Corner[kFace[f][2]],
                            Smile::Vec4{ col, 1.0f });
                DD.Triangle(Corner[kFace[f][0]], Corner[kFace[f][2]], Corner[kFace[f][3]],
                            Smile::Vec4{ col, 1.0f });
            }
        }
    }

    void GizmoController::Submit(Smile::Renderer& _Renderer) {
        if (!Enabled) return;

        SubmitLightShapes(_Renderer);

        Vec3 Pivot; int Idx; bool IsLight;
        const bool HasSelection = GetPivot(_Renderer, Pivot, Idx, IsLight);
        // Mantém as restrições da UI sincronizadas mesmo sem handles.
        SelectionIsLight = HasSelection && IsLight;

        if (Mode == EMode::Select) return;
        if (!HasSelection) return;
        // Rotação e escala mantêm o pivô fixo; translação acompanha o objeto.
        if (Dragging && Mode != EMode::Translate) Pivot = DragStartPivot;
        const float Scale = ScaleFor(_Renderer, Pivot);

        Vec3 Axes[3];
        AxisBasis(_Renderer, IsLight, Idx, Axes);

        switch (Mode) {
            case EMode::Translate: SubmitTranslate(_Renderer, Pivot, Scale, Axes); break;
            case EMode::Rotate:
                if (HasRotationHandles(_Renderer, IsLight, Idx))
                    SubmitRotate(_Renderer, Pivot, Scale, Axes);
                break;
            case EMode::Scale:
                if (!IsLight) SubmitScale(_Renderer, Pivot, Scale, Axes);
                break;
            default: break;
        }
    }

    bool GizmoController::HasRotationHandles(Smile::Renderer& R, bool IsLight, int Idx) const {
        if (!IsLight) return true;
        const auto& Lights = R.GetScene().Lights();
        if (Idx < 0 || Idx >= static_cast<int>(Lights.size())) return false;
        return Lights[static_cast<size_t>(Idx)].Type == Smile::ELightType::Spot;
    }

    void GizmoController::MarkLightEnergyChanged(Smile::Renderer& R) const {
        // Invalidação coalescida evita flush por evento durante o arraste.
        R.Settings().MarkSceneContentDirty();
    }

    void GizmoController::CommitRenderableEdit(Smile::Renderer& R, int Idx,
                                               const Vec3& OldMin, const Vec3& OldMax) const {
        auto& List = R.GetScene().Renderables();
        if (Idx < 0 || Idx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(Idx)];
        Rn.RefreshWorldBounds();
        R.GetScene().BumpTransformsVersion();
        // Invalida os volumes anterior e atual para remover iluminação residual.
        R.NotifyGIRegionChanged(OldMin, OldMax, Smile::EGIRegionChange::Geometry);
        R.NotifyGIRegionChanged(Rn.AABBMin, Rn.AABBMax, Smile::EGIRegionChange::Geometry);
    }

    bool GizmoController::OnMousePress(Smile::Renderer& R, u32 X, u32 Y) {
        if (!Enabled || Mode == EMode::Select) return false;
        Vec3 RingPoint{};
        const EAxis Axis = (Mode == EMode::Rotate) ? HitTestRings(R, X, Y, &RingPoint)
                                                   : HitTestAxes(R, X, Y);
        if (Axis == EAxis::None) return false;
        Vec3 O, Dir;
        if (!R.ScreenToRay(X, Y, O, Dir)) return false;
        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(R, Pivot, Idx, IsLight)) return false;

        Vec3 Axes[3];
        AxisBasis(R, IsLight, Idx, Axes);

        Active         = Axis;
        Hovered        = Axis;
        Dragging       = true;
        DragIdx        = Idx;
        DragIsLight    = IsLight;
        DragStartPivot = Pivot;
        // Desenho e matemática compartilham a base congelada.
        for (int i = 0; i < 3; ++i) DragAxes[i] = Axes[i];
        DragAxisWorld  = AxisFromEnum(Axes, Axis).NormalizedSafe(Vec3::UnitY());
        DragAngle      = 0.0f;

        if (IsLight) {
            const Smile::FLight& L = R.GetScene().Lights()[static_cast<size_t>(Idx)];
            DragStartPos     = L.Position;
            DragStartSpotDir = L.Direction;
        } else {
            const Smile::FRenderable& Rn = R.GetScene().Renderables()[static_cast<size_t>(Idx)];
            DragStartPos    = Rn.Transform.Position;
            DragStartEuler  = Rn.Transform.RotationEuler;
            DragStartScale  = Rn.Transform.Scale;
        }

        if (Mode == EMode::Rotate) {
            if (!BeginDial(R, X, Y, RingPoint)) { Dragging = false; Active = EAxis::None; return false; }
        } else {
            DragStartT   = AxisParam(O, Dir, DragAxisWorld, Pivot);
            DragGizmoLen = ScaleFor(R, Pivot);
        }

        // Durante o gesto, o CSM trata o renderizável como dinâmico.
        if (!IsLight && Idx >= 0) {
            const auto& List = R.GetScene().Renderables();
            if (Idx < static_cast<int>(List.size())) {
                const Smile::FRenderable& Rn = List[static_cast<size_t>(Idx)];
                R.SetDraggingRenderable(Rn.Id);
                if (Rn.Mobility == Smile::EMobility::Static)
                    R.GetScene().BumpStaticCastersVersion();
            }
        }
        return true;
    }

    bool GizmoController::BeginDial(Smile::Renderer& R, u32 X, u32 Y, const Vec3& RingPoint) {
        float px0, py0;
        if (!R.WorldToScreen(DragStartPivot, px0, py0)) return false;
        const Vec2 Off{ static_cast<float>(X) - px0, static_cast<float>(Y) - py0 };
        const float Len = std::sqrt(Off.X*Off.X + Off.Y*Off.Y);
        if (Len < 1e-3f) return false;
        DialRef         = Vec2{ Off.X / Len, Off.Y / Len };
        DialAngle       = 0.0f;
        DialRevolutions = 0;
        const Vec3 RingVec = RingPoint - DragStartPivot;
        DragStartRingDir   = RingVec.NormalizedSafe(Vec3::UnitX());

        // Mede em tela o sinal da rotação para absorver handedness e orientação do eixo.
        float ax, ay, bx, by;
        const Vec3 Probe = DragStartPivot
                         + Mat44::RotationAxis(DragAxisWorld, 0.02f).TransformVectorRow(RingVec);
        if (!R.WorldToScreen(DragStartPivot + RingVec, ax, ay)) return false;
        if (!R.WorldToScreen(Probe, bx, by)) return false;
        const Vec2 Radial{ ax - px0, ay - py0 };
        const Vec2 Perp{ -Radial.Y, Radial.X };
        const float Along = (bx - ax) * Perp.X + (by - ay) * Perp.Y;
        DialSign = (Along < 0.0f) ? -1.0f : 1.0f;
        return true;
    }

    void GizmoController::OnMouseMove(Smile::Renderer& R, u32 X, u32 Y) {
        if (!Enabled || Mode == EMode::Select) return;
        if (!Dragging) { Hovered = HitTest(R, X, Y); return; }
        if (DragIdx < 0) return;

        switch (Mode) {
            case EMode::Translate: ApplyTranslate(R, X, Y); break;
            case EMode::Rotate:    ApplyRotate(R, X, Y);    break;
            case EMode::Scale:     ApplyScale(R, X, Y);     break;
            default: break;
        }
    }

    void GizmoController::ApplyTranslate(Smile::Renderer& R, u32 X, u32 Y) {
        Vec3 O, Dir;
        if (!R.ScreenToRay(X, Y, O, Dir)) return;
        const float T = AxisParam(O, Dir, DragAxisWorld, DragStartPivot);
        float Delta = T - DragStartT;

        if (SnapActive()) {
            // Em mundo, alinha o pivô à grade; em local, quantiza a distância percorrida.
            if (SpaceFor(DragIsLight) == ESpace::World) {
                const float PivotOnAxis = DragStartPivot.Dot(DragAxisWorld) + Delta;
                Delta += Smile::SnapToStep(PivotOnAxis, SnapCfg.TranslateM) - PivotOnAxis;
            } else {
                Delta = Smile::SnapToStep(Delta, SnapCfg.TranslateM);
            }
        }

        const Vec3 NewPos = DragStartPos + DragAxisWorld * Delta;

        if (DragIsLight) {
            auto& Lights = R.GetScene().Lights();
            if (DragIdx >= static_cast<int>(Lights.size())) return;
            Smile::FLight& L = Lights[static_cast<size_t>(DragIdx)];
            // Invalida as regiões anterior e atual da luz.
            Vec3 OldMin, OldMax, NewMin, NewMax;
            L.InfluenceBounds(OldMin, OldMax);
            L.Position = NewPos;
            L.InfluenceBounds(NewMin, NewMax);
            R.NotifyGIRegionChanged(OldMin, OldMax, Smile::EGIRegionChange::Radiometric);
            R.NotifyGIRegionChanged(NewMin, NewMax, Smile::EGIRegionChange::Radiometric);
            MarkLightEnergyChanged(R);
            return;
        }

        auto& List = R.GetScene().Renderables();
        if (DragIdx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(DragIdx)];
        const Vec3 OldMin = Rn.AABBMin, OldMax = Rn.AABBMax;
        Rn.Transform.Position = NewPos;
        CommitRenderableEdit(R, DragIdx, OldMin, OldMax);
    }

    void GizmoController::ApplyRotate(Smile::Renderer& R, u32 X, u32 Y) {
        float px0, py0;
        // A câmera pode se mover durante o gesto.
        if (!R.WorldToScreen(DragStartPivot, px0, py0)) return;
        const Vec2 Off{ static_cast<float>(X) - px0, static_cast<float>(Y) - py0 };
        if (Off.X*Off.X + Off.Y*Off.Y < 1e-6f) return;

        // O dial em tela acumula voltas mesmo com o anel de perfil.
        const Vec2 Perp{ -DialRef.Y, DialRef.X };
        const float a = std::atan2(Off.X*Perp.X + Off.Y*Perp.Y, Off.X*DialRef.X + Off.Y*DialRef.Y);
        if (a * DialAngle < 0.0f && std::fabs(a) > Smile::HalfPi)
            DialRevolutions += (a < 0.0f) ? 1 : -1;
        DialAngle = a;
        DragAngle = (a + Smile::TwoPi * static_cast<float>(DialRevolutions)) * DialSign;
        // Snap atua no ângulo do gesto, não nos componentes Euler resultantes.
        if (SnapActive())
            DragAngle = Smile::SnapToStep(DragAngle, SnapCfg.RotateDeg * Smile::ToRad);

        const Mat44 Delta = Mat44::RotationAxis(DragAxisWorld, DragAngle);

        if (DragIsLight) {
            auto& Lights = R.GetScene().Lights();
            if (DragIdx >= static_cast<int>(Lights.size())) return;
            Smile::FLight& L = Lights[static_cast<size_t>(DragIdx)];
            Vec3 Min, Max;
            L.InfluenceBounds(Min, Max);
            L.Direction = Delta.TransformVectorRow(DragStartSpotDir)
                               .NormalizedSafe(DragStartSpotDir);
            R.NotifyGIRegionChanged(Min, Max, Smile::EGIRegionChange::Radiometric);
            MarkLightEnergyChanged(R);
            return;
        }

        auto& List = R.GetScene().Renderables();
        if (DragIdx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(DragIdx)];
        const Vec3 OldMin = Rn.AABBMin, OldMax = Rn.AABBMax;
        // Recalcula do estado inicial para evitar deriva e manter o pivô fixo.
        Rn.Transform.RotationEuler =
            (Mat44::RotationEulerXYZ(DragStartEuler) * Delta).ToEulerXYZ();
        Rn.Transform.Position = Delta.TransformVectorRow(DragStartPos - DragStartPivot)
                              + DragStartPivot;
        CommitRenderableEdit(R, DragIdx, OldMin, OldMax);
    }

    void GizmoController::ApplyScale(Smile::Renderer& R, u32 X, u32 Y) {
        if (DragIsLight) return;
        Vec3 O, Dir;
        if (!R.ScreenToRay(X, Y, O, Dir)) return;
        auto& List = R.GetScene().Renderables();
        if (DragIdx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(DragIdx)];

        // Um comprimento de handle corresponde a duplicar a escala, independente da câmera.
        const float T   = AxisParam(O, Dir, DragAxisWorld, DragStartPivot);
        const int   i   = IndexFromEnum(Active);
        const float Raw = 1.0f + (T - DragStartT) / DragGizmoLen;

        const float Start  = Comp(DragStartScale, i);
        float       Target = std::max(kMinScale, Start * Raw);
        // Snap atua no valor final e respeita o piso invertível.
        if (SnapActive())
            Target = std::max(kMinScale, Smile::SnapToStep(Target, SnapCfg.ScaleStep));
        const float Eff    = (std::fabs(Start) > 1e-8f) ? (Target / Start) : 1.0f;

        const Vec3 OldMin = Rn.AABBMin, OldMax = Rn.AABBMax;
        Vec3 NewScale = DragStartScale;
        Comp(NewScale, i) = Target;

        // Compensa a translação para manter o pivô visual imóvel.
        const Mat44 Rot = Mat44::RotationEulerXYZ(Rn.Transform.RotationEuler);
        Vec3 DLocal = Rot.TransformVectorRowTransposed(DragStartPivot - DragStartPos);
        Comp(DLocal, i) *= Eff;

        Rn.Transform.Scale    = NewScale;
        Rn.Transform.Position = DragStartPivot - Rot.TransformVectorRow(DLocal);
        CommitRenderableEdit(R, DragIdx, OldMin, OldMax);
    }

    bool GizmoController::OnMouseRelease(Smile::Renderer& R) {
        bool Edited = false;
        // Ao terminar, devolve o objeto ao cache estático do CSM.
        if (Dragging && !DragIsLight && R.GetDraggingRenderable() != 0) {
            const Smile::FRenderable* Rn = R.GetScene().FindRenderable(R.GetDraggingRenderable());
            if (Rn && Rn->Mobility == Smile::EMobility::Static)
                R.GetScene().BumpStaticCastersVersion();
            // Clique sem deslocamento não deve sujar a camada autorada.
            auto Differs = [](const Vec3& A, const Vec3& B) {
                return std::fabs(A.X-B.X) > 1e-6f || std::fabs(A.Y-B.Y) > 1e-6f
                    || std::fabs(A.Z-B.Z) > 1e-6f;
            };
            Edited = Rn && (Differs(Rn->Transform.Position,      DragStartPos)
                         || Differs(Rn->Transform.RotationEuler, DragStartEuler)
                         || Differs(Rn->Transform.Scale,         DragStartScale));
        }
        R.SetDraggingRenderable(0);

        Dragging    = false;
        Active      = EAxis::None;
        DragIdx     = -1;
        DragIsLight = false;
        DragAngle   = 0.0f;
        return Edited;
    }
}
