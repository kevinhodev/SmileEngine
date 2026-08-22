#pragma once

#include "Smile/Graphics/Debug/FrameCapture.h"
#include "Smile/Graphics/GI/RadianceCache.h"

namespace Smile {
    // Sessao deterministica e dados efetivos registrados no manifesto de captura.
    struct FRendererCaptureState {
        FFrameCapture Session;

        Vec3 SunDirection{ 0.0f, 1.0f, 0.0f };
        f32  SunHours           = 0.0f;
        f32  PinnedHoursApplied = -1.0f;

        bool SetupGuard             = false;
        bool ReGIRRanThisFrame      = false;
        bool RRRanThisFrame         = false;
        u32  GILightCountThisFrame  = 0;

        FRadianceCacheStats CacheStats{};

        static constexpr f32 kDeltaSeconds   = 1.0f / 60.0f;
        static constexpr f32 kElapsedSeconds = 0.0f;
    };
}
