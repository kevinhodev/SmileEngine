#pragma once

#include "Smile/Graphics/Scene/Camera.h"
#include "Smile/Scene/Scene.h"
#include <vector>

namespace Smile {
    // Estado CPU persistente que descreve a cena observada pelo renderer.
    struct FRendererSceneState {
        FCamera Camera;
        FScene  Scene;

        // O Id sobrevive a mudancas nas listas; o indice e apenas o cache do frame atual.
        FSceneObjectRef Selection;
        // Bounds definidos pelo loader e reutilizados por rebuilds que nao recarregam a cena.
        Vec3 BoundsMin{ 0.0f, 0.0f, 0.0f };
        Vec3 BoundsMax{ 0.0f, 0.0f, 0.0f };

        u64 DraggingRenderableId       = 0;
        u64 TlasTransformsVersion      = 0;
        u64 MeshLightTransformsVersion = 0;

        bool TlasFlagsDirty         = false;
        bool MaterialRTStateDirty   = false;
        // Uma ENTRADA de bake de micro-map mudou (alpha, cutoff, textura de BaseColor, toggle de
        // alpha test). Separado do MaterialRTStateDirty porque o custo e outra ordem de grandeza:
        // aquele sobe uma linha de 64 B, este rebakeia o par e reconstroi BLAS.
        //
        // Marcado no COMMIT da edicao, e nao por tick de slider: arrastar o corte de alpha numa
        // folhagem dispararia um rebake por movimento do mouse. Quem publica isto e o
        // MaterialsBridge quando o controle e solto.
        bool OmmInputsDirty         = false;
        bool MeshLightEmissiveDirty = false;
        bool IndirectLightingDirty  = false;
        bool SceneContentDirty      = false;
    };
}
