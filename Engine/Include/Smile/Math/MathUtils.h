#pragma once

#include "Smile/Core/Types.h"
#include <cmath>

namespace Smile {

constexpr f32 Pi    = 3.14159265358979f;
constexpr f32 TwoPi = 6.28318530717959f;
constexpr f32 HalfPi = 1.57079632679490f;
constexpr f32 ToRad = Pi / 180.0f;
constexpr f32 ToDeg = 180.0f / Pi;

inline f32  Clamp(f32 v, f32 lo, f32 hi) { return v < lo ? lo : v > hi ? hi : v; }
inline f32  Lerp(f32 a, f32 b, f32 t)    { return a + t * (b - a); }
inline f32  Saturate(f32 v)               { return Clamp(v, 0.0f, 1.0f); }
inline bool IsNearZero(f32 v)             { return v > -1e-6f && v < 1e-6f; }

} // namespace Smile
