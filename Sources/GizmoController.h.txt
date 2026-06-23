#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Core/Types.h"

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Logica do gizmo de TRANSLACAO — vive no EDITOR (ferramenta de editor, nao runtime). Usa os
    // servicos que a Engine expoe: FDebugDraw (desenha as setas), WorldToScreen/ScreenToRay (camera)
    // e GetScene() (le/escreve o Transform do selecionado). Pivo = centro da AABB de mundo do
    // selecionado (meshes world-baked com Transform identidade). Hit-test = distancia 2D em tela
    // (estilo Cry); arraste = ray<->eixo ponto-mais-proximo (estilo Cry/Unreal).
    class GizmoController {
    public:
        enum class EAxis : Smile::u32 { None = 0, X, Y, Z };

        void SetEnabled(bool V) { Enabled = V; }
        bool GetEnabled() const { return Enabled; }
        bool IsDragging() const { return Dragging; }

        // Chamado a cada frame APOS UpdateCamera e ANTES de RenderFrame: submete as 3 setas ao
        // FDebugDraw da Engine (no-op se desabilitado / sem selecao).
        void Submit(Smile::Renderer& R);

        // Eventos de mouse (coords em pixels do backbuffer = coords logicas do widget):
        // press tenta pegar um handle -> retorna true se comecou um arraste (editor NAO faz picking).
        bool OnMousePress(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void OnMouseMove(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);   // drag ou hover
        void OnMouseRelease();

    private:
        // Pivo (centro da AABB do selecionado) + escala constante-em-tela. -1 em OutIdx = sem selecao.
        bool   GetPivot(Smile::Renderer& R, Smile::Vec3& OutPivot, int& OutIdx) const;
        float  ScaleFor(Smile::Renderer& R, const Smile::Vec3& Pivot) const;
        EAxis  HitTest(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const;  // distancia 2D aos 3 eixos
        // t do ponto do eixo (Pivot + t*AxisDir) mais proximo do raio (AxisDir e Dir unitarios).
        float  AxisParam(const Smile::Vec3& Origin, const Smile::Vec3& Dir,
                         const Smile::Vec3& AxisDir, const Smile::Vec3& Pivot) const;

        bool        Enabled = true;
        EAxis       Hovered = EAxis::None;
        EAxis       Active  = EAxis::None;
        bool        Dragging = false;
        int         DragIdx = -1;            // indice do renderavel sendo arrastado
        Smile::Vec3 DragStartPos{};          // Transform.Position no inicio do arraste
        Smile::Vec3 DragStartPivot{};        // pivo FIXO durante o arraste (linha do eixo)
        float       DragStartT = 0.0f;       // offset inicial ao longo do eixo
    };
}
