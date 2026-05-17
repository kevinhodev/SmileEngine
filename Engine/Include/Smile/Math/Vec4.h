#pragma once

#include "Smile/Math/Vec3.h"

namespace Smile {

template<typename T>
struct TVec4 {
    T X{}, Y{}, Z{}, W{};

    TVec4() = default;
    constexpr TVec4(T x, T y, T z, T w) : X(x), Y(y), Z(z), W(w) {}
    explicit constexpr TVec4(T v)        : X(v), Y(v), Z(v), W(v) {}
    explicit constexpr TVec4(const TVec3<T>& v, T w = T(0)) : X(v.X), Y(v.Y), Z(v.Z), W(w) {}

    static TVec4 Zero() { return {T(0), T(0), T(0), T(0)}; }

    TVec4  operator+(const TVec4& v) const { return {X+v.X, Y+v.Y, Z+v.Z, W+v.W}; }
    TVec4  operator-(const TVec4& v) const { return {X-v.X, Y-v.Y, Z-v.Z, W-v.W}; }
    TVec4  operator*(T s)            const { return {X*s,   Y*s,   Z*s,   W*s};   }

    TVec3<T> XYZ() const { return {X, Y, Z}; }

    const T* Data() const { return &X; }
};

using Vec4  = TVec4<f32>;
using Vec4i = TVec4<i32>;

} // namespace Smile
