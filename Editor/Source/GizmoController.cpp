#include "SmileEditor/GizmoController.h"
#include "Smile/Graphics/Renderer.h"
// O Renderer.h so declara o FRenderSettings; o MarkSceneContentDirty precisa da definicao.
#include "Smile/Graphics/RenderSettings.h"
#include "Smile/Scene/Scene.h"
#include <algorithm>
#include <cmath>

namespace SmileEditor {
    using Smile::Vec2;
    using Smile::Vec3;
    using Smile::Mat44;
    using Smile::u32;

    namespace {
        constexpr float kSizeFrac  = 0.18f;  // tamanho do handle ~18% da meia-altura da tela
        constexpr float kHitRadius = 9.0f;   // pixels
        constexpr float kIconFrac  = 0.22f;  // icone da luz = 22% do handle (~44px de altura)
        // Espessura em pixels (constante em tela). A haste da seta era 1px serrilhado; 3px com
        // AA e o que deixa o gizmo com o peso do da UE e facil de mirar.
        constexpr float kAxisThickness = 3.0f;
        constexpr float kSpotThickness = 2.0f;
        constexpr float kRingThickness = 2.5f;
        // Anel um pouco menor que a seta: os dois modos leem com o mesmo peso em tela e sobra
        // margem pro anel de tela (rotacao na vista) que a fase dos handles de plano traz.
        constexpr float kRingRadiusFrac = 0.85f;
        constexpr int   kRingSegments   = 64;
        constexpr float kTwoPi          = 6.28318530718f;
        // Piso da escala: o transform continua invertivel e a AABB nao degenera. Escala negativa
        // (espelho) nao e representavel aqui — inverteria a orientacao das normais sem que nada
        // no pipeline soubesse.
        constexpr float kMinScale = 0.001f;

        Vec3 AxisFromEnum(const Vec3 Axes[3], GizmoController::EAxis A) {
            return A == GizmoController::EAxis::X ? Axes[0]
                 : A == GizmoController::EAxis::Y ? Axes[1] : Axes[2];
        }
        int IndexFromEnum(GizmoController::EAxis A) {
            return A == GizmoController::EAxis::X ? 0
                 : A == GizmoController::EAxis::Y ? 1 : 2;
        }
        // Componente por indice (0=X, 1=Y, 2=Z). Nao e (&V.X)[i]: o TVec3 sao tres membros
        // nomeados, nao um array, e passear com ponteiro de um subobjeto para o vizinho e UB —
        // funciona hoje e some num rebuild com outro nivel de otimizacao.
        float& Comp(Vec3& V, int I) { return I == 0 ? V.X : (I == 1 ? V.Y : V.Z); }
        float  Comp(const Vec3& V, int I) { return I == 0 ? V.X : (I == 1 ? V.Y : V.Z); }

        // Distancia 2D de um ponto ao SEGMENTO (nao a reta): sem o clamp, o eixo/anel continuaria
        // capturando o cursor muito depois da ponta desenhada.
        float DistPointSeg(float Px, float Py, float Ax, float Ay, float Bx, float By) {
            const float dx = Bx - Ax, dy = By - Ay;
            const float len2 = dx * dx + dy * dy;
            float t = (len2 > 1e-6f) ? (((Px - Ax) * dx + (Py - Ay) * dy) / len2) : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float cx = Ax + dx * t, cy = Ay + dy * t;
            return std::sqrt((Px - cx) * (Px - cx) + (Py - cy) * (Py - cy));
        }

        // --- Rotacao no formato do FTransform -----------------------------------------------
        // FONTE UNICA de convencao: monta com os MESMOS Mat44::RotationX/Y/Z que o
        // FTransform::Matrix usa, na mesma ordem (linha, R = Rx*Ry*Rz). Nao reimplementar as
        // matrizes aqui — o RotationY desta engine gira no sentido oposto ao dos outros dois, e
        // uma copia "arrumada" divergiria silenciosamente do que a cena renderiza.
        Mat44 RotOf(const Vec3& Euler) {
            return Mat44::RotationX(Euler.X) * Mat44::RotationY(Euler.Y)
                 * Mat44::RotationZ(Euler.Z);
        }

        // Inverso exato do RotOf. Mesma extracao que o cooker faz no DecomposeToEngineTRS: os
        // dois tem de concordar, senao um objeto rotacionado no editor e re-cozinhado sai torto.
        Vec3 EulerOf(const Mat44& R) {
            const float sy = std::clamp(R.M[0][2], -1.0f, 1.0f);
            const float ry = std::asin(sy);
            // cos(ry) ~ 0 e gimbal lock: o par (rx, rz) fica ambiguo, mas a MATRIZ resultante
            // continua correta, que e o que importa pra renderizar.
            if (std::fabs(std::cos(ry)) > 1e-6f)
                return Vec3{ std::atan2(R.M[1][2], R.M[2][2]), ry,
                             std::atan2(R.M[0][1], R.M[0][0]) };
            return Vec3{ std::atan2(-R.M[2][1], R.M[1][1]), ry, 0.0f };
        }

        // Rodrigues na convencao LINHA (v' = v*M), regra da mao direita em volta de A unitario.
        // E a transposta da forma classica de coluna — confere com o RotationX/RotationZ desta
        // engine; o RotationY nao bate, e nao precisa: quem le o resultado e o EulerOf.
        Mat44 AxisAngleRow(const Vec3& A, float Angle) {
            const float c = std::cos(Angle), s = std::sin(Angle), k = 1.0f - c;
            Mat44 m = Mat44::Identity();
            m.M[0][0] = c + k*A.X*A.X;     m.M[0][1] = k*A.X*A.Y + s*A.Z; m.M[0][2] = k*A.X*A.Z - s*A.Y;
            m.M[1][0] = k*A.Y*A.X - s*A.Z; m.M[1][1] = c + k*A.Y*A.Y;     m.M[1][2] = k*A.Y*A.Z + s*A.X;
            m.M[2][0] = k*A.Z*A.X + s*A.Y; m.M[2][1] = k*A.Z*A.Y - s*A.X; m.M[2][2] = c + k*A.Z*A.Z;
            return m;
        }

        // v * M (linha), so a parte 3x3. O Mat44::operator*(Vec4) e da convencao COLUNA e daria
        // a transposta — o mesmo cuidado que o RefreshWorldBounds documenta.
        Vec3 MulRow3(const Vec3& V, const Mat44& M) {
            return Vec3{ V.X*M.M[0][0] + V.Y*M.M[1][0] + V.Z*M.M[2][0],
                         V.X*M.M[0][1] + V.Y*M.M[1][1] + V.Z*M.M[2][1],
                         V.X*M.M[0][2] + V.Y*M.M[1][2] + V.Z*M.M[2][2] };
        }

        // v * M^T (linha) = as tres projecoes de v nas LINHAS de M. Com M ortonormal e o mesmo
        // que v * M^-1: leva um vetor de mundo para o espaco local do objeto.
        Vec3 MulRow3Transposed(const Vec3& V, const Mat44& M) {
            return Vec3{ V.Dot(M.GetRow3(0)), V.Dot(M.GetRow3(1)), V.Dot(M.GetRow3(2)) };
        }
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

    void GizmoController::AxisBasis(Smile::Renderer& R, bool IsLight, int Idx,
                                    Vec3 Out[3]) const {
        // Congelada no press: ver o comentario do DragAxes. Vale para os tres modos.
        if (Dragging) { for (int i = 0; i < 3; ++i) Out[i] = DragAxes[i]; return; }

        Out[0] = Vec3::UnitX(); Out[1] = Vec3::UnitY(); Out[2] = Vec3::UnitZ();
        // SpaceFor com o IsLight RECEBIDO, e nao o EffectiveSpace() que le o cache: um evento de
        // mouse pode chegar com uma selecao mais nova do que a do ultimo Submit. A regra em si
        // (luz e escala) mora la, num lugar so.
        if (SpaceFor(IsLight) == ESpace::World || Idx < 0) return;
        const auto& List = R.GetScene().Renderables();
        if (Idx >= static_cast<int>(List.size())) return;
        // Linha i da matriz de rotacao (convencao linha) = imagem do eixo local i em mundo.
        const Mat44 Rot = RotOf(List[static_cast<size_t>(Idx)].Transform.RotationEuler);
        for (int i = 0; i < 3; ++i) Out[i] = Rot.GetRow3(i).NormalizedSafe(Out[i]);
    }

    GizmoController::EAxis GizmoController::HitTestAxes(Smile::Renderer& _Renderer,
                                                        u32 _X, u32 _Y) const {
        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(_Renderer, Pivot, Idx, IsLight)) return EAxis::None;
        if (Mode == EMode::Scale && IsLight) return EAxis::None; // luz nao tem escala
        const float Scale = ScaleFor(_Renderer, Pivot);
        float px0, py0;
        if (!_Renderer.WorldToScreen(Pivot, px0, py0)) return EAxis::None;

        Vec3 Axes[3];
        AxisBasis(_Renderer, IsLight, Idx, Axes);
        const EAxis Ids[3] = { EAxis::X, EAxis::Y, EAxis::Z };
        const float fx = static_cast<float>(_X), fy = static_cast<float>(_Y);
        float Best = kHitRadius; EAxis BestAxis = EAxis::None;
        for (int i = 0; i < 3; ++i) {
            float px1, py1;
            if (!_Renderer.WorldToScreen(Pivot + Axes[i] * Scale, px1, py1)) continue;
            const float dist = DistPointSeg(fx, fy, px0, py0, px1, py1);
            if (dist < Best) { Best = dist; BestAxis = Ids[i]; }
        }
        return BestAxis;
    }

    GizmoController::EAxis GizmoController::HitTestRings(Smile::Renderer& _Renderer,
                                                         u32 _X, u32 _Y,
                                                         Vec3* OutRingPoint) const {
        Vec3 Pivot; int Idx; bool IsLight;
        if (!GetPivot(_Renderer, Pivot, Idx, IsLight)) return EAxis::None;
        // Luz pontual nao tem orientacao: girar nao mudaria nada. Spot sim (a direcao).
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

        float Best = kHitRadius; EAxis BestAxis = EAxis::None;
        for (int i = 0; i < 3; ++i) {
            const Vec3 U = Axes[(i + 1) % 3], V = Axes[(i + 2) % 3];
            float PrevX = 0.0f, PrevY = 0.0f; bool PrevOk = false;
            for (int s = 0; s <= kRingSegments; ++s) {
                const float  a = kTwoPi * static_cast<float>(s) / kRingSegments;
                const Vec3   Offset = U * std::cos(a) + V * std::sin(a);
                const Vec3   W  = Pivot + Offset * Radius;
                float cx, cy;
                const bool Ok = _Renderer.WorldToScreen(W, cx, cy);
                // So a metade voltada pra camera responde ao cursor: e a que esta desenhada
                // solida, e em vista quase de topo a metade de tras passa a poucos pixels do
                // cursor e roubaria o clique do anel que o usuario esta vendo.
                const bool Near = Offset.Dot(CamDir) > 0.0f;
                if (Ok && PrevOk && Near) {
                    const float dist = DistPointSeg(fx, fy, PrevX, PrevY, cx, cy);
                    if (dist < Best) {
                        Best = dist; BestAxis = Ids[i];
                        if (OutRingPoint) *OutRingPoint = W;
                    }
                }
                PrevX = cx; PrevY = cy; PrevOk = Ok;
            }
        }
        return BestAxis;
    }

    GizmoController::EAxis GizmoController::HitTest(Smile::Renderer& R, u32 X, u32 Y) const {
        switch (Mode) {
            case EMode::Translate:
            case EMode::Scale:  return HitTestAxes(R, X, Y);
            case EMode::Rotate: return HitTestRings(R, X, Y, nullptr);
            default:            return EAxis::None; // Select: nenhum handle existe
        }
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
            // Ponta (piramide de 4 faces). A base sai dos OUTROS dois eixos do gizmo, entao ela
            // acompanha de graca quando a base virar local (seletor Global/Local).
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
            // Durante o arraste so o anel pego continua na tela: os outros dois viram ruido em
            // cima do angulo que o usuario esta lendo.
            if (Dragging && Highlighted != Ids[i]) continue;
            const Vec3  col = (Highlighted == Ids[i]) ? kHighlight : kAxisColor[i];
            const Vec3  U = Axes[(i + 1) % 3], V = Axes[(i + 2) % 3];
            Vec3 Prev = Pivot + U * Radius;
            bool PrevNear = U.Dot(CamDir) > 0.0f;
            for (int s = 1; s <= kRingSegments; ++s) {
                const float a = kTwoPi * static_cast<float>(s) / kRingSegments;
                const Vec3  Offset = U * std::cos(a) + V * std::sin(a);
                const Vec3  W = Pivot + Offset * Radius;
                const bool  Near = Offset.Dot(CamDir) > 0.0f;
                // A metade de tras fica fantasma em vez de sumir: sem ela o anel vira um arco
                // solto e a orientacao do plano de giro se perde de vista.
                const bool  Solid = Near && PrevNear;
                DD.Line(Prev, W, Smile::Vec4{ col, Solid ? 1.0f : 0.22f },
                        Smile::EDebugDepthMode::Foreground,
                        Solid ? kRingThickness : 1.5f);
                Prev = W; PrevNear = Near;
            }
        }

        // Feedback do gesto: raio de onde comecou + raio de onde esta. Le o angulo percorrido
        // sem precisar de texto na tela (o FDebugDraw nao desenha texto).
        if (Dragging && Active != EAxis::None) {
            // O raio branco e onde o gesto COMECOU (o ponto do anel que foi pego); o amarelo e
            // onde ele esta agora — girar o inicial pelo angulo acumulado da o atual. Os dois
            // saem do raio ATUAL do anel, entao acompanham se a camera se aproximar no meio.
            const Vec3 Start = DragStartRingDir * Radius;
            const Vec3 Cur   = MulRow3(Start, AxisAngleRow(DragAxisWorld, DragAngle));
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
        // Faces do cubo por indice de canto (bit0 = eixo, bit1 = u, bit2 = v).
        static const int kFace[6][4] = { {0,2,6,4}, {1,5,7,3}, {0,4,5,1},
                                         {2,3,7,6}, {0,1,3,2}, {4,6,7,5} };

        for (int i = 0; i < 3; ++i) {
            const Vec3 d   = Axes[i];
            const Vec3 col = (Highlighted == Ids[i]) ? kHighlight : kAxisColor[i];
            DD.Line(Pivot, Pivot + d * ShaftEnd, Smile::Vec4{ col, 1.0f },
                    Smile::EDebugDepthMode::Foreground, kAxisThickness);

            // Ponta = cubo (contra a piramide do mover): a silhueta diz qual ferramenta esta
            // ativa mesmo quando o gizmo esta pequeno na tela.
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

        // Visualizacao das luzes independe do modo e de haver selecao (markers sempre visiveis):
        // no modo Selecionar eles sao o unico alvo clicavel de uma luz.
        SubmitLightShapes(_Renderer);

        Vec3 Pivot; int Idx; bool IsLight;
        const bool HasSelection = GetPivot(_Renderer, Pivot, Idx, IsLight);
        // Cache para as consultas SEM argumento (o botao de espaco da toolbar). Atualizado ANTES
        // do early-return do modo Selecionar: o botao continua na tela ali, e mostrar a restricao
        // certa nao depende de haver handle desenhado. Sem selecao nao ha restricao — o botao
        // volta a mostrar a escolha do usuario.
        SelectionIsLight = HasSelection && IsLight;

        if (Mode == EMode::Select) return;
        if (!HasSelection) return;
        // Congelar o pivo durante o arraste vale para ROTACIONAR e ESCALAR, e so para eles: os
        // dois giram/crescem EM VOLTA de um ponto que tem de ficar parado, e a AABB de mundo
        // deles muda de forma no meio do gesto (a caixa de um objeto girado nao e a imagem da
        // caixa original), entao recentrar por frame arrastaria o anel para longe do cursor.
        //
        // MOVER e o oposto: a caixa se desloca RIGIDAMENTE junto com o objeto, e o gizmo tem de
        // ir junto. Congelar ali deixava as setas paradas no lugar de origem durante todo o
        // arraste, pulando para o objeto so no release.
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
        // Luz pontual e isotropica: nao ha o que girar. Spot tem direcao.
        return Lights[static_cast<size_t>(Idx)].Type == Smile::ELightType::Spot;
    }

    void GizmoController::MarkLightEnergyChanged(Smile::Renderer& R) const {
        // O NotifyGIRegionChanged cobre so o que tem granularidade ESPACIAL (o atlas do DDGI).
        // Reservoirs, reflexoes, ReGIR e o cache de radiancia nao tem — e o contrato do
        // Renderer::NotifyGIRegionChanged diz explicitamente que quem muda ENERGIA tambem chama
        // isto. Mover ou apontar uma luz muda energia; mover um OBJETO nao precisa, porque la os
        // filtros reprojetam por motion vector (por isso o CommitRenderableEdit nao chama).
        //
        // Mark e nao Notify: o Mark e coalescido e existe justamente para o caminho de arraste,
        // onde um Notify imediato custaria um Flush da fila por frame do gesto. E o mesmo par que
        // o LightsBridge::InvalidateLightRegion usa — funil de TODA edicao de luz pelo painel.
        R.Settings().MarkSceneContentDirty();
    }

    void GizmoController::CommitRenderableEdit(Smile::Renderer& R, int Idx,
                                               const Vec3& OldMin, const Vec3& OldMax) const {
        auto& List = R.GetScene().Renderables();
        if (Idx < 0 || Idx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(Idx)];
        // Recomputa da caixa LOCAL em vez de somar o passo na de mundo. O remendo antigo so valia
        // porque o gizmo era de translacao pura; com rotacao ou escala somar o incremento
        // descreveria um volume que nao e o do objeto — e o sintoma seria geometria sumindo no
        // culling, longe daqui.
        Rn.RefreshWorldBounds();
        R.GetScene().BumpTransformsVersion(); // TLAS segue o objeto (rebuild leve no frame)
        // Caixa de ONDE SAIU e de ONDE CHEGOU: sem a antiga, o color bleed do objeto fica
        // estampado no lugar de onde ele saiu — a "luz fantasma" que so sumia quando a histerese
        // terminava de escoar.
        R.NotifyGIRegionChanged(OldMin, OldMax, Smile::EGIRegionChange::Geometry);
        R.NotifyGIRegionChanged(Rn.AABBMin, Rn.AABBMax, Smile::EGIRegionChange::Geometry);
        // Sem MarkSceneContentDirty aqui, e de proposito: transformar e um gesto CONTINUO e os
        // historicos de tela reprojetam por motion vector (o objeto esta no TemporalMotion),
        // entao disoclusao e clamp de vizinhanca rejeitam o historico invalido sozinhos em
        // alguns frames. Derrubar reservoir a cada frame do arraste seria reset permanente
        // durante o gesto — exatamente o que HistoryDomain.h descreve como o erro antigo.
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
        // Congela a base ANTES de qualquer coisa mexer no objeto — dali em diante desenho e
        // matematica leem a mesma, inclusive no espaco Local, onde o objeto gira sob o gizmo.
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

    bool GizmoController::BeginDial(Smile::Renderer& R, u32 X, u32 Y, const Vec3& RingPoint) {
        float px0, py0;
        if (!R.WorldToScreen(DragStartPivot, px0, py0)) return false;
        const Vec2 Off{ static_cast<float>(X) - px0, static_cast<float>(Y) - py0 };
        const float Len = std::sqrt(Off.X*Off.X + Off.Y*Off.Y);
        if (Len < 1e-3f) return false;   // cursor em cima do pivo: nao ha direcao de referencia
        DialRef         = Vec2{ Off.X / Len, Off.Y / Len };
        DialAngle       = 0.0f;
        DialRevolutions = 0;
        // Guardado unitario: quem desenha multiplica pelo raio DAQUELE frame, entao o raio de
        // feedback nao descola do anel se a camera se mexer durante o gesto.
        const Vec3 RingVec = RingPoint - DragStartPivot;
        DragStartRingDir   = RingVec.NormalizedSafe(Vec3::UnitX());

        // Sinal do mostrador, MEDIDO em vez de deduzido: gira o ponto pego por um angulo
        // positivo minusculo e ve pra que lado ele anda NA TELA. Resolve de uma vez a
        // combinacao de eixo apontando pra longe da camera, projecao esquerda e Y de tela pra
        // baixo — cada uma delas invertendo o sentido por conta propria. A sonda usa o vetor
        // com o COMPRIMENTO real do anel: a um unico metro do pivo os dois pontos cairiam no
        // mesmo pixel num objeto distante, e o sinal viria de ruido de arredondamento.
        float ax, ay, bx, by;
        const Vec3 Probe = DragStartPivot + MulRow3(RingVec, AxisAngleRow(DragAxisWorld, 0.02f));
        if (!R.WorldToScreen(DragStartPivot + RingVec, ax, ay)) return false;
        if (!R.WorldToScreen(Probe, bx, by)) return false;
        // Direcao de "angulo de tela crescente" no ponto pego: perpendicular ao raio, girada
        // 90 graus no mesmo sentido em que o atan2 abaixo mede.
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
        const Vec3 NewPos = DragStartPos + DragAxisWorld * (T - DragStartT);

        if (DragIsLight) {
            auto& Lights = R.GetScene().Lights();
            if (DragIdx >= static_cast<int>(Lights.size())) return;
            Smile::FLight& L = Lights[static_cast<size_t>(DragIdx)];
            // Volume de influencia de ONDE SAIU + de ONDE CHEGOU: a luz deixa de iluminar um e
            // passa a iluminar o outro, e as sondas dos dois precisam reavaliar. Duas chamadas
            // em vez de uniao na mao — o FDDGI une, e a uniao dele e crua (sem padding
            // acumulado), o que e o que torna seguro chamar isto a cada frame do arraste.
            Vec3 OldMin, OldMax, NewMin, NewMax;
            L.InfluenceBounds(OldMin, OldMax);
            L.Position = NewPos;
            L.InfluenceBounds(NewMin, NewMax);
            // Radiometrico: a luz anda, a GEOMETRIA nao. As sondas dos dois volumes recebem
            // energia diferente, mas continuam vendo exatamente as mesmas superficies — nao ha
            // o que reclassificar (ver EGIRegionChange).
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
        // Projecao do pivo lida AGORA, nao no press: a camera pode voar durante o arraste, e o
        // centro do mostrador tem de continuar sendo o pivo que aparece na tela.
        if (!R.WorldToScreen(DragStartPivot, px0, py0)) return;
        const Vec2 Off{ static_cast<float>(X) - px0, static_cast<float>(Y) - py0 };
        if (Off.X*Off.X + Off.Y*Off.Y < 1e-6f) return;

        // Mostrador portado do CDialInteraction da Cry (Gizmos/AxisRotateGizmo.cpp): angulo do
        // cursor na base ancorada na direcao do press, com
        // contagem de VOLTAS. Em tela, e nao no plano do anel, porque o plano fica degenerado
        // exatamente quando o anel esta de perfil — e la o arraste tem de continuar respondendo.
        const Vec2 Perp{ -DialRef.Y, DialRef.X };
        const float a = std::atan2(Off.X*Perp.X + Off.Y*Perp.Y, Off.X*DialRef.X + Off.Y*DialRef.Y);
        if (a * DialAngle < 0.0f && std::fabs(a) > 1.5707963f)
            DialRevolutions += (a < 0.0f) ? 1 : -1;
        DialAngle = a;
        DragAngle = (a + kTwoPi * static_cast<float>(DialRevolutions)) * DialSign;

        const Mat44 Delta = AxisAngleRow(DragAxisWorld, DragAngle);

        if (DragIsLight) {
            auto& Lights = R.GetScene().Lights();
            if (DragIdx >= static_cast<int>(Lights.size())) return;
            Smile::FLight& L = Lights[static_cast<size_t>(DragIdx)];
            // O pivo E a posicao da luz, entao ela nao anda: so a direcao do cone gira. Volume
            // de influencia idem — muda pra onde aponta, nao onde alcanca.
            Vec3 Min, Max;
            L.InfluenceBounds(Min, Max);
            L.Direction = MulRow3(DragStartSpotDir, Delta).NormalizedSafe(DragStartSpotDir);
            R.NotifyGIRegionChanged(Min, Max, Smile::EGIRegionChange::Radiometric);
            MarkLightEnergyChanged(R);
            return;
        }

        auto& List = R.GetScene().Renderables();
        if (DragIdx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(DragIdx)];
        const Vec3 OldMin = Rn.AABBMin, OldMax = Rn.AABBMax;
        // Sempre a partir do estado do PRESS (nunca somando no atual): o gesto inteiro e uma
        // funcao do angulo total, entao voltar o cursor ao ponto de partida devolve exatamente
        // a rotacao original, sem deriva acumulada.
        //   p_mundo = p_local*S*Rot + t   =>  girar tudo em volta de P da
        //   Rot' = Rot*Delta   e   t' = (t - P)*Delta + P
        Rn.Transform.RotationEuler = EulerOf(RotOf(DragStartEuler) * Delta);
        Rn.Transform.Position      = MulRow3(DragStartPos - DragStartPivot, Delta) + DragStartPivot;
        CommitRenderableEdit(R, DragIdx, OldMin, OldMax);
    }

    void GizmoController::ApplyScale(Smile::Renderer& R, u32 X, u32 Y) {
        if (DragIsLight) return; // luz nao tem escala (o hit-test ja barra, isto e cinto)
        Vec3 O, Dir;
        if (!R.ScreenToRay(X, Y, O, Dir)) return;
        auto& List = R.GetScene().Renderables();
        if (DragIdx >= static_cast<int>(List.size())) return;
        Smile::FRenderable& Rn = List[static_cast<size_t>(DragIdx)];

        // Razao RELATIVA ao tamanho do handle em tela: arrastar um comprimento inteiro de handle
        // dobra a escala, e o gesto se comporta igual perto e longe da camera. Um delta em
        // metros (o que a Flax faz, com um 0.01 fixo) depende da distancia e da resolucao.
        const float T   = AxisParam(O, Dir, DragAxisWorld, DragStartPivot);
        const int   i   = IndexFromEnum(Active);
        const float Raw = 1.0f + (T - DragStartT) / DragGizmoLen;

        // UMA razao para as duas contas. O piso e aplicado na ESCALA FINAL, e a compensacao do
        // pivo tem de usar a razao que de fato saiu dele — antes a escala era clampada e o pivo
        // seguia com a razao crua, entao um objeto que ja nascia abaixo de 1 batia no piso e
        // DESLIZAVA (a escala parava, a translacao continuava andando).
        const float Start  = Comp(DragStartScale, i);
        const float Target = std::max(kMinScale, Start * Raw);
        const float Eff    = (std::fabs(Start) > 1e-8f) ? (Target / Start) : 1.0f;

        const Vec3 OldMin = Rn.AABBMin, OldMax = Rn.AABBMax;
        Vec3 NewScale = DragStartScale;
        Comp(NewScale, i) = Target;

        // Mantem o PIVO parado. A origem do objeto (Transform.Position) quase nunca coincide com
        // o centro da caixa, entao escalar sem corrigir a translacao faria o objeto deslizar.
        //   d = P - t  (mundo)  ->  d_local = d*Rot^T  ->  escala componente i  ->  volta
        const Mat44 Rot = RotOf(Rn.Transform.RotationEuler);
        Vec3 DLocal = MulRow3Transposed(DragStartPivot - DragStartPos, Rot);
        Comp(DLocal, i) *= Eff;

        Rn.Transform.Scale    = NewScale;
        Rn.Transform.Position = DragStartPivot - MulRow3(DLocal, Rot);
        CommitRenderableEdit(R, DragIdx, OldMin, OldMax);
    }

    bool GizmoController::OnMouseRelease(Smile::Renderer& R) {
        bool Edited = false;
        // O objeto volta ao conjunto estatico no lugar NOVO, entao o mapa cacheado precisa ser
        // refeito uma vez. Bumpa mesmo que o arraste nao tenha movido nada (clique sem
        // deslocamento): o custo e um redesenho, e a alternativa e comparar transforms para
        // economizar um caso que nao acontece em sessao real.
        if (Dragging && !DragIsLight && R.GetDraggingRenderable() != 0) {
            const Smile::FRenderable* Rn = R.GetScene().FindRenderable(R.GetDraggingRenderable());
            if (Rn && Rn->Mobility == Smile::EMobility::Static)
                R.GetScene().BumpStaticCastersVersion();
            // O DIRTY, ao contrario do bump acima, e comparado: um clique parado no handle
            // ainda passa uma vez pelo Apply (o release traz a posicao do mouse junto), e sujar
            // a camada autorada por isso faria o editor perguntar "salvar?" sem haver o que
            // salvar. Aqui vale a pena a comparacao — ela decide um dialogo, nao um redesenho.
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
