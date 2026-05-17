#pragma once

#include "Smile/Math/MathUtils.h"
#include <cmath>

namespace Smile {

template<typename T>
struct TVec2 {
    T X{}, Y{};

    TVec2() = default;
    constexpr TVec2(T x, T y) : X(x), Y(y) {}
    explicit constexpr TVec2(T v) : X(v), Y(v) {}

    static TVec2 Zero()  { return {T(0), T(0)}; }
    static TVec2 One()   { return {T(1), T(1)}; }

    TVec2  operator+(const TVec2& v) const { return {X+v.X, Y+v.Y}; }
    TVec2  operator-(const TVec2& v) const { return {X-v.X, Y-v.Y}; }
    TVec2  operator*(T s)            const { return {X*s,   Y*s};   }
    TVec2  operator/(T s)            const { return {X/s,   Y/s};   }
    TVec2  operator-()               const { return {-X, -Y};        }
    TVec2& operator+=(const TVec2& v)      { X+=v.X; Y+=v.Y; return *this; }
    TVec2& operator*=(T s)                 { X*=s;   Y*=s;   return *this; }

    T     Dot(const TVec2& v)  const { return X*v.X + Y*v.Y; }
    T     LengthSq()           const { return Dot(*this); }
    T     Length()             const { return std::sqrt(LengthSq()); }
    TVec2 Normalized()         const { return *this * (T(1) / Length()); }
    TVec2 NormalizedSafe(const TVec2& fallback = Zero()) const {
        T len = Length();
        return len > T(1e-6) ? *this * (T(1) / len) : fallback;
    }

    const T* Data() const { return &X; }
};

using Vec2  = TVec2<f32>;
using Vec2d = TVec2<f64>;
using Vec2i = TVec2<i32>;

} // namespace Smile
