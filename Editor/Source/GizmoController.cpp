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
        constexpr float kSizeFrac  = 0.18f;  // tamanho da seta ~18% da meia-altura da tela
        constexpr float kHitRadius = 9.0f;   // pixels
        constexpr float kIconFrac  = 0.22f;  // icone da luz = 22% da seta (~44px de altura)
        // Espessura em pixels (constante em tela). A haste da seta era 1px serrilhado; 3px com
        // AA e o que deixa o gizmo com o peso do da UE e facil de mirar.
        constexpr float kAxisThickness = 3.0f;
        constexpr float kSpotThickness = 2.0f;
    }

    float GizmoController::IconHalfSizeFor(Smile::Renderer& R, const Vec3& Position) const {
        return ScaleFor(R, Position) * kIconFrac;
    }

    int GizmoController::PickLightIcon(Smile::Renderer& R, u32 _X, u32 _Y) const {
        // Gizmo desligado nao desenha icone nenhum (Submit sai cedo); marker invisivel nao pode
        // interceptar o clique que deveria cair no picking normal de objetos.
        if (!Enabled) return -1;
        const auto& Lights = R.GetScene().Lights();
        if (Lights.empty()) return -1;

        // Raio MEDIDO, nao constante: projeta o centro e a borda do billboard e usa a distancia
        // entre os dois em pixels. O raio antigo (22px fixo) so batia com o desenho a 1080p — o
        // icone escala com a altura do viewport (~0,0198*H), entao a 1440p a area clicavel ficava
        // menor que o glifo e a 720p, maior.
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

            // Icones sobrepostos: ganha o ULTIMO, porque e o que aparece por cima — o
            // SubmitLightShapes desenha na ordem do vetor, alpha-blend e sem depth. Isso NAO e
            // oclusao: icone e sempre visivel por decisao de design, entao uma luz atras da
            // parede continua clicavel (como deve ser). O criterio e "o de cima ganha", e ele
            // acompanha de graca quando os icones ganharem sort por profundidade — basta a
            // ordem de submissao mudar, que desenho e picking mudam juntos.
            BestIdx = i;
        }
        return BestIdx;
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
        // FOV vem do Renderer (fonte unica de quem monta a Projection), nao de um literal local.
        const float Dist = (Pivot - R.GetCameraPos()).Length();
        return std::max(0.001f, Dist * std::tan(R.GetFovY() * 0.5f) * kSizeFrac);
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

            // Icone billboard (lampada/spot, glifo SDF) em TODAS as luzes — cor da propria luz,
            // cinza quando apagada; a selecionada ganha o anel branco. Tamanho constante em
            // tela (ScaleFor ja escala por distancia), ~44px de altura a 1080p (calibrado na
            // referencia dos viewport icons do Flax). E o alvo do clique de selecao.
            // O wireframe do volume (esfera/cone) saiu de cena por ora — decisao do usuario
            // 2026-07-10; quando voltar, e Line(..., EDebugDepthMode::Scene) pra ele parar no
            // chao em vez de atravessar.
            const float S = IconHalfSizeFor(R, L.Position);
            const Vec3 MCol = L.Enabled
                ? Vec3{ std::max(L.Color.X, 0.15f), std::max(L.Color.Y, 0.15f),
                        std::max(L.Color.Z, 0.15f) }
                : Vec3{ 0.35f, 0.35f, 0.35f };
            const bool IsSel = (i == Selected);
            DD.Icon(L.Position, S, MCol,
                    L.Type == Smile::ELightType::Spot ? 1u : 0u, IsSel);

            // Spot selecionado: toco curto indicando a direcao (unico feedback do apontamento
            // sem o wire do volume; some junto com a selecao).
            if (IsSel && L.Type == Smile::ELightType::Spot) {
                const Vec3 Dir = L.Direction.NormalizedSafe(Vec3{ 0.0f, -1.0f, 0.0f });
                DD.Line(L.Position + Dir * (S * 1.4f), L.Position + Dir * (S * 4.0f),
                        Smile::Vec4{ MCol, 1.0f },
                        Smile::EDebugDepthMode::Foreground, kSpotThickness);
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
            DebugDraw.Line(Pivot, Pivot + d * ShaftEnd, Smile::Vec4{ col, 1.0f },
                           Smile::EDebugDepthMode::Foreground, kAxisThickness);
            // Ponta (piramide de 4 faces).
            const Vec3 u = (i == 0) ? Vec3::UnitY() : Vec3::UnitX();
            const Vec3 v = (i == 2) ? Vec3::UnitY() : Vec3::UnitZ();
            const Vec3 tip = Pivot + d * Len;
            const Vec3 bc  = Pivot + d * ShaftEnd;
            const Vec3 c[4] = { bc + u*HeadR + v*HeadR, bc - u*HeadR + v*HeadR,
                                bc - u*HeadR - v*HeadR, bc + u*HeadR - v*HeadR };
            for (int s = 0; s < 4; ++s)
                DebugDraw.Triangle(tip, c[s], c[(s + 1) & 3], Smile::Vec4{ col, 1.0f });
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

        // Enquanto arrasta, o renderavel entra no conjunto DINAMICO do CSM (ver
        // Renderer::SetDraggingRenderable). O bump aqui e o que o tira do mapa estatico uma
        // vez; sem ele o objeto continuaria estampado na posicao antiga do mapa cacheado
        // enquanto a copia dinamica se move — sombra dupla.
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
        Rn.Transform.Position = NewPos;
        // Recomputa da caixa LOCAL em vez de somar o passo na de mundo. O remendo antigo so
        // valia porque o gizmo e de translacao pura; no dia em que ele ganhar rotacao ou escala,
        // somar o incremento descreveria um volume que nao e o do objeto — e o sintoma seria
        // geometria sumindo no culling, longe daqui.
        Rn.RefreshWorldBounds();
        R.GetScene().BumpTransformsVersion(); // TLAS segue o objeto (rebuild leve no frame)
    }

    void GizmoController::OnMouseRelease(Smile::Renderer& R) {
        // O objeto volta ao conjunto estatico no lugar NOVO, entao o mapa cacheado precisa ser
        // refeito uma vez. Bumpa mesmo que o arraste nao tenha movido nada (clique sem
        // deslocamento): o custo e um redesenho, e a alternativa e comparar transforms para
        // economizar um caso que nao acontece em sessao real.
        if (Dragging && !DragIsLight && R.GetDraggingRenderable() != 0) {
            const Smile::FRenderable* Rn = R.GetScene().FindRenderable(R.GetDraggingRenderable());
            if (Rn && Rn->Mobility == Smile::EMobility::Static)
                R.GetScene().BumpStaticCastersVersion();
        }
        R.SetDraggingRenderable(0);

        Dragging    = false;
        Active      = EAxis::None;
        DragIdx     = -1;
        DragIsLight = false;
    }
}
