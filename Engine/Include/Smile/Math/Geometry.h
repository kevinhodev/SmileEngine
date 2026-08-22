#pragma once

#include "Smile/Math/Vec2.h"
#include <algorithm>
#include <type_traits>

namespace Smile {
    template<typename T>
    TVec2<T> ClosestPointOnSegment(const TVec2<T>& Point,
                                   const TVec2<T>& Start,
                                   const TVec2<T>& End,
                                   T* OutParameter = nullptr) {
        static_assert(std::is_floating_point_v<T>);
        const TVec2<T> Segment = End - Start;
        const T LengthSq = Segment.LengthSq();
        const T Parameter = LengthSq > T(0)
            ? std::clamp((Point - Start).Dot(Segment) / LengthSq, T(0), T(1))
            : T(0);
        if (OutParameter) *OutParameter = Parameter;
        return Start + Segment * Parameter;
    }
}
