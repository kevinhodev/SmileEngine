#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Core/Types.h"

namespace Smile { class Renderer; }

namespace SmileEditor {
    // Logica do gizmo de TRANSLACAO — vive no EDITOR (ferramenta de editor, nao runtime). Usa os
    // servicos que a Engine expoe: FDebugDraw (desenha as setas), WorldToScreen/ScreenToRay (camera)
    // e GetScene() (le/escreve o Transform do selecionado). Pivo = a posicao da LUZ selecionada
    // (prioridade; o editor mantem selecao de luz e de renderavel exclusivas), senao o centro da
    // AABB de mundo do renderavel selecionado (meshes world-baked com Transform identidade).
    // Hit-test = distancia 2D em tela (estilo Cry); arraste = ray<->eixo ponto-mais-proximo
    // (estilo Cry/Unreal).
    class GizmoController {
    public:
        enum class EAxis : Smile::u32 { None = 0, X, Y, Z };

        void SetEnabled(bool V) { Enabled = V; }
        bool GetEnabled() const { return Enabled; }
        bool IsDragging() const { return Dragging; }

        // Chamado a cada frame APOS UpdateCamera e ANTES de RenderFrame: submete as 3 setas ao
        // FDebugDraw da Engine (no-op se desabilitado / sem selecao) + a visualizacao das luzes
        // (marker em todas; esfera do raio / cone do spot na selecionada).
        void Submit(Smile::Renderer& R);

        // Eventos de mouse (coords em pixels do backbuffer = coords logicas do widget):
        // press tenta pegar um handle -> retorna true se comecou um arraste (editor NAO faz picking).
        bool OnMousePress(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);
        void OnMouseMove(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y);   // drag ou hover
        void OnMouseRelease();

    private:
        // Pivo (posicao da luz selecionada, senao centro da AABB do renderavel selecionado) +
        // escala constante-em-tela. -1 em OutIdx = sem selecao; OutIsLight diz qual lista.
        bool   GetPivot(Smile::Renderer& R, Smile::Vec3& OutPivot, int& OutIdx,
                        bool& OutIsLight) const;
        float  ScaleFor(Smile::Renderer& R, const Smile::Vec3& Pivot) const;
        EAxis  HitTest(Smile::Renderer& R, Smile::u32 X, Smile::u32 Y) const;  // distancia 2D aos 3 eixos
        // t do ponto do eixo (Pivot + t*AxisDir) mais proximo do raio (AxisDir e Dir unitarios).
        float  AxisParam(const Smile::Vec3& Origin, const Smile::Vec3& Dir,
                         const Smile::Vec3& AxisDir, const Smile::Vec3& Pivot) const;
        void   SubmitLightShapes(Smile::Renderer& R) const; // markers + esfera/cone da selecionada

        bool        Enabled = true;
        EAxis       Hovered = EAxis::None;
        EAxis       Active  = EAxis::None;
        bool        Dragging = false;
        int         DragIdx = -1;            // indice do renderavel/luz sendo arrastado
        bool        DragIsLight = false;     // DragIdx aponta pra Scene.Lights() em vez de Renderables()
        Smile::Vec3 DragStartPos{};          // posicao no inicio do arraste
        Smile::Vec3 DragStartPivot{};        // pivo FIXO durante o arraste (linha do eixo)
        float       DragStartT = 0.0f;       // offset inicial ao longo do eixo
    };
}
