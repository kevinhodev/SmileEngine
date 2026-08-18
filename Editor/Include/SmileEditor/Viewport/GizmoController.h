#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Core/Types.h"

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Gizmo de transformação do editor. Renderizáveis usam o centro da AABB em mundo como
    // pivô para manter meshes importadas com origem distante estáveis durante rotação e escala.
    class GizmoController {
    public:
        enum class EAxis : Smile::u32 { None = 0, X, Y, Z };

        // Select desativa os handles, mas preserva o picking de objetos e luzes.
        enum class EMode : Smile::u32 { Select = 0, Translate, Rotate, Scale };

        void SetEnabled(bool V) { Enabled = V; }
        bool GetEnabled() const { return Enabled; }
        bool IsDragging() const { return Dragging; }

        // O modo permanece fixo durante um gesto.
        void  SetMode(EMode M) { if (!Dragging) { Mode = M; Hovered = EAxis::None; } }

        enum class ESpace : Smile::u32 { World = 0, Local };

        // Explica à UI por que o espaço escolhido não pode ser aplicado.
        enum class ESpaceRestriction : Smile::u32 { None = 0, ScaleIsLocal, LightHasNoLocal };

        // Luzes não possuem uma base local completa; escala permanece local porque TRS não
        // representa o cisalhamento produzido por escala em eixos globais rotacionados.
        ESpace SpaceFor(bool IsLight) const {
            if (IsLight)              return ESpace::World;
            if (Mode == EMode::Scale) return ESpace::Local;
            return Space;
        }
        ESpaceRestriction RestrictionFor(bool IsLight) const {
            if (IsLight)              return ESpaceRestriction::LightHasNoLocal;
            if (Mode == EMode::Scale) return ESpaceRestriction::ScaleIsLocal;
            return ESpaceRestriction::None;
        }

        // Espaço escolhido pelo usuário; EffectiveSpace aplica as restrições do contexto atual.
        ESpace GetSpace() const { return Space; }
        void   SetSpace(ESpace S) {
            if (Dragging || IsSpaceForced()) return;
            Space   = S;
            Hovered = EAxis::None;
        }

        ESpace            EffectiveSpace() const { return SpaceFor(SelectionIsLight); }
        ESpaceRestriction Restriction()    const { return RestrictionFor(SelectionIsLight); }
        bool              IsSpaceForced()  const { return Restriction() != ESpaceRestriction::None; }
        EMode GetMode() const { return Mode; }

        // Snap absoluto: o resultado, calculado a partir do estado inicial do gesto, cai em
        // múltiplos do passo configurado.
        struct FSnap {
            bool  TranslateOn = false;
            bool  RotateOn    = false;
            bool  ScaleOn     = false;
            float TranslateM  = 0.10f;   // metros
            float RotateDeg   = 15.0f;
            float ScaleStep   = 0.10f;   // fracao (0.10 = 10%)
        };

        const FSnap& Snap() const { return SnapCfg; }
        void SetSnapTranslateOn(bool V) { SnapCfg.TranslateOn = V; }
        void SetSnapRotateOn(bool V)    { SnapCfg.RotateOn    = V; }
        void SetSnapScaleOn(bool V)     { SnapCfg.ScaleOn     = V; }
        void SetSnapTranslateM(float V) { if (V > 0.0f) SnapCfg.TranslateM = V; }
        void SetSnapRotateDeg(float V)  { if (V > 0.0f) SnapCfg.RotateDeg  = V; }
        void SetSnapScaleStep(float V)  { if (V > 0.0f) SnapCfg.ScaleStep  = V; }

        // Ctrl inverte temporariamente o estado de snap do modo atual.
        void SetSnapInverted(bool V) { SnapInverted = V; }

        bool SnapActive() const {
            const bool On = Mode == EMode::Translate ? SnapCfg.TranslateOn
                          : Mode == EMode::Rotate    ? SnapCfg.RotateOn
                          : Mode == EMode::Scale     ? SnapCfg.ScaleOn
                                                     : false;
            return On != SnapInverted;
        }

        // Deve rodar após UpdateCamera e antes de RenderFrame.
        void Submit(Smile::Renderer& R);

        // Luzes não entram no ID-buffer; o teste usa a mesma métrica de tela dos billboards.
        // Em sobreposição, vence o último ícone submetido. Retorna -1 quando não há acerto.
        int PickLightIcon(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const;

        // Coordenadas de mouse correspondem aos pixels do backbuffer.
        bool OnMousePress(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void OnMouseMove(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        // Retorna true somente quando o gesto alterou um renderizável autorado.
        bool OnMouseRelease(Smile::Renderer& R);

    private:
        // Retorna o pivô selecionado e a escala constante em tela. OutIdx=-1 indica ausência.
        bool   GetPivot(Smile::Renderer& R, Smile::Vec3& OutPivot, int& OutIdx,
                        bool& OutIsLight) const;
        float  ScaleFor(Smile::Renderer& R, const Smile::Vec3& Pivot) const;

        void   AxisBasis(Smile::Renderer& R, bool IsLight, int Idx, Smile::Vec3 Out[3]) const;

        EAxis  HitTestAxes(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const;
        // Testa apenas a metade do anel voltada à câmera e devolve o ponto mais próximo.
        EAxis  HitTestRings(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y,
                            Smile::Vec3* OutRingPoint) const;
        EAxis  HitTest(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const;

        float  AxisParam(const Smile::Vec3& Origin, const Smile::Vec3& Dir,
                         const Smile::Vec3& AxisDir, const Smile::Vec3& Pivot) const;

        void   SubmitTranslate(Smile::Renderer& R, const Smile::Vec3& Pivot, float Scale,
                               const Smile::Vec3 Axes[3]) const;
        void   SubmitRotate(Smile::Renderer& R, const Smile::Vec3& Pivot, float Scale,
                            const Smile::Vec3 Axes[3]) const;
        void   SubmitScale(Smile::Renderer& R, const Smile::Vec3& Pivot, float Scale,
                           const Smile::Vec3 Axes[3]) const;
        void   SubmitLightShapes(Smile::Renderer& R) const;
        bool   HasRotationHandles(Smile::Renderer& R, bool IsLight, int Idx) const;
        // Fonte única do tamanho usado pelo desenho e pelo hit-test do ícone.
        float  IconHalfSizeFor(Smile::Renderer& R, const Smile::Vec3& Position) const;

        // Invalida históricos globais afetados por mudanças de iluminação.
        void   MarkLightEnergyChanged(Smile::Renderer& R) const;

        // Atualiza bounds, TLAS e regiões de GI após alterar um renderizável.
        void   CommitRenderableEdit(Smile::Renderer& R, int Idx,
                                    const Smile::Vec3& OldMin, const Smile::Vec3& OldMax) const;

        // Retorna false para um gesto degenerado ou com pivô fora da tela.
        bool   BeginDial(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y,
                         const Smile::Vec3& RingPoint);
        void   ApplyTranslate(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void   ApplyRotate(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void   ApplyScale(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);

        bool        Enabled = true;
        EMode       Mode    = EMode::Translate;
        ESpace      Space   = ESpace::World;
        // Cache para consultas da UI que não possuem acesso ao Renderer.
        bool        SelectionIsLight = false;
        FSnap       SnapCfg{};
        bool        SnapInverted = false;
        EAxis       Hovered = EAxis::None;
        EAxis       Active  = EAxis::None;
        bool        Dragging = false;
        int         DragIdx = -1;
        bool        DragIsLight = false;
        Smile::Vec3 DragStartPos{};
        Smile::Vec3 DragStartPivot{};
        Smile::Vec3 DragAxisWorld{};
        // Pivô e base permanecem fixos para todo o gesto.
        Smile::Vec3 DragAxes[3]{};
        float       DragStartT = 0.0f;

        // A rotação é sempre recomputada do estado inicial para evitar deriva incremental.
        Smile::Vec3 DragStartEuler{};
        Smile::Vec3 DragStartSpotDir{};
        Smile::Vec3 DragStartRingDir{};
        Smile::Vec2 DialRef{};
        float       DialAngle = 0.0f;
        int         DialRevolutions = 0;
        float       DialSign = 1.0f;
        float       DragAngle = 0.0f;

        Smile::Vec3 DragStartScale{ 1.0f, 1.0f, 1.0f };
        float       DragGizmoLen = 1.0f;
    };
}
