#pragma once

#include "Smile/Math/MathUtils.h"
#include <cmath>

namespace Smile {

template<typename T>
struct TVec3 {
    T X{}, Y{}, Z{};

    TVec3() = default;
    constexpr TVec3(T x, T y, T z) : X(x), Y(y), Z(z) {}
    explicit constexpr TVec3(T v)   : X(v), Y(v), Z(v) {}

    static TVec3 Zero()  { return {T(0), T(0), T(0)}; }
    static TVec3 One()   { return {T(1), T(1), T(1)}; }
    static TVec3 UnitX() { return {T(1), T(0), T(0)}; }
    static TVec3 UnitY() { return {T(0), T(1), T(0)}; }
    static TVec3 UnitZ() { return {T(0), T(0), T(1)}; }

    TVec3  operator+(const TVec3& v) const { return {X+v.X, Y+v.Y, Z+v.Z}; }
    TVec3  operator-(const TVec3& v) const { return {X-v.X, Y-v.Y, Z-v.Z}; }
    TVec3  operator*(T s)            const { return {X*s,   Y*s,   Z*s};   }
    TVec3  operator/(T s)            const { return {X/s,   Y/s,   Z/s};   }
    TVec3  operator-()               const { return {-X, -Y, -Z};          }
    TVec3& operator+=(const TVec3& v)      { X+=v.X; Y+=v.Y; Z+=v.Z; return *this; }
    TVec3& operator-=(const TVec3& v)      { X-=v.X; Y-=v.Y; Z-=v.Z; return *this; }
    TVec3& operator*=(T s)                 { X*=s;   Y*=s;   Z*=s;   return *this; }

    T     Dot(const TVec3& v)  const { return X*v.X + Y*v.Y + Z*v.Z; }
    TVec3 Cross(const TVec3& v) const {
        return {Y*v.Z - Z*v.Y,
                Z*v.X - X*v.Z,
                X*v.Y - Y*v.X};
    }

    T     LengthSq() const { return Dot(*this); }
    T     Length()   const { return std::sqrt(LengthSq()); }

    TVec3 Normalized() const { return *this * (T(1) / Length()); }

    // Padrão CryEngine: retorna fallback se o vetor for degenrado
    TVec3 NormalizedSafe(const TVec3& fallback = Zero()) const {
        T len = Length();
        return len > T(1e-6) ? *this * (T(1) / len) : fallback;
    }

    // Padrão CryEngine: escolhe o melhor eixo perpendicular automaticamente
    TVec3 GetOrthogonal() const {
        return std::abs(X) > std::abs(Z)
            ? TVec3{-Y, X, T(0)}.Normalized()
            : TVec3{T(0), -Z, Y}.Normalized();
    }

    const T* Data() const { return &X; }
};

using Vec3  = TVec3<f32>;
using Vec3d = TVec3<f64>;
using Vec3i = TVec3<i32>;

} // namespace Smile
