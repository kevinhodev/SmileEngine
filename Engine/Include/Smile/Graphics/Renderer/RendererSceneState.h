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
        std::vector<Mat44> PreviousModels;

        // Bounds definidos pelo loader e reutilizados por rebuilds que nao recarregam a cena.
        Vec3 BoundsMin{ 0.0f, 0.0f, 0.0f };
        Vec3 BoundsMax{ 0.0f, 0.0f, 0.0f };

        u64 DraggingRenderableId       = 0;
        u64 TlasTransformsVersion      = 0;
        u64 MeshLightTransformsVersion = 0;

        bool TlasFlagsDirty         = false;
        bool MaterialRTStateDirty   = false;
        bool MeshLightEmissiveDirty = false;
        bool IndirectLightingDirty  = false;
        bool SceneContentDirty      = false;
    };
}
