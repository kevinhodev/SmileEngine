#pragma once

#include "Smile/Graphics/GI/IndirectPolicy.h"
#include "Smile/Math/Math.h"
#include <unordered_map>

namespace Smile {
    // Estado CPU que conecta o frame atual aos historicos temporais do renderer.
    struct FRendererFrameState {
        Mat44 LastViewProj{};
        Mat44 PrevViewProj{};
        Mat44 PrevViewProjNoTranslation{};
        Mat44 NrdPrevView{};
        Mat44 NrdPrevProj{};

        Vec3 PrevCameraPosition{ 0.0f, 0.0f, 0.0f };
        Vec2 PrevJitterUv{ 0.0f, 0.0f };
        Vec2 PrevJitterPx{ 0.0f, 0.0f };

        // Posicao anterior por identidade estavel para motion vectors de sombras locais.
        std::unordered_map<u64, Vec3> PreviousDirectLightPositions;

        FEffectiveIndirectPolicy PrevIndirectPolicy;
        bool HasPrevIndirectPolicy = false;
        bool TAARanLastFrame       = false;

        f32 ElapsedTime   = 0.0f;
        f32 LastDeltaTime = 0.0f;

        // FrameIndex e monotonico; TemporalSampleIndex pode reiniciar em capturas deterministicas.
        u32 FrameIndex          = 0;
        u32 TemporalSampleIndex = 0;
    };
}
