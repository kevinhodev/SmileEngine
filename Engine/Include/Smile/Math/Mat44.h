#pragma once

#include "Smile/Math/Vec4.h"
#include <cmath>
#include <cstring>

namespace Smile {
    template<typename T>
    struct TMat44 {
        T M[4][4];

        static TMat44 Identity() {
            TMat44 m{};
            m.M[0][0] = m.M[1][1] = m.M[2][2] = m.M[3][3] = T(1);
            return m;
        }

        static TMat44 RotationX(T angle) {
            TMat44 m = Identity();
            T c = std::cos(angle), s = std::sin(angle);
            m.M[1][1] =  c;  m.M[1][2] = s;
            m.M[2][1] = -s;  m.M[2][2] = c;
            return m;
        }

        static TMat44 RotationY(T angle) {
            TMat44 m = Identity();
            T c = std::cos(angle), s = std::sin(angle);
            m.M[0][0] =  c;  m.M[0][2] = s;
            m.M[2][0] = -s;  m.M[2][2] = c;
            return m;
        }

        static TMat44 RotationZ(T angle) {
            TMat44 m = Identity();
            T c = std::cos(angle), s = std::sin(angle);
            m.M[0][0] =  c;  m.M[0][1] = s;
            m.M[1][0] = -s;  m.M[1][1] = c;
            return m;
        }

        static TMat44 Translation(const TVec3<T>& t) {
            TMat44 m = Identity();
            m.M[3][0] = t.X;
            m.M[3][1] = t.Y;
            m.M[3][2] = t.Z;
            return m;
        }

        static TMat44 Scale(const TVec3<T>& s) {
            TMat44 m = Identity();
            m.M[0][0] = s.X;
            m.M[1][1] = s.Y;
            m.M[2][2] = s.Z;
            return m;
        }

        // Projeção perspectiva LH com FOV vertical em radianos
        static TMat44 PerspectiveFovLH(T fovY, T aspect, T nearZ, T farZ) {
            T h = T(1) / std::tan(fovY * T(0.5));
            T w = h / aspect;
            T q = farZ / (farZ - nearZ);

            TMat44 m{};
            m.M[0][0] = w;
            m.M[1][1] = h;
            m.M[2][2] = q;
            m.M[2][3] = T(1);
            m.M[3][2] = -nearZ * q;
            return m;
        }

        static TMat44 LookAtLH(const TVec3<T>& eye, const TVec3<T>& target, const TVec3<T>& up) {
            TVec3<T> f = (target - eye).Normalized();   // forward
            TVec3<T> r = up.Cross(f).Normalized();       // right = cross(up, forward)
            TVec3<T> u = f.Cross(r);                     // corrected up = cross(forward, right)

            TMat44 m = Identity();
            m.M[0][0] = r.X;  m.M[0][1] = u.X;  m.M[0][2] = f.X;
            m.M[1][0] = r.Y;  m.M[1][1] = u.Y;  m.M[1][2] = f.Y;
            m.M[2][0] = r.Z;  m.M[2][1] = u.Z;  m.M[2][2] = f.Z;
            m.M[3][0] = -r.Dot(eye);
            m.M[3][1] = -u.Dot(eye);
            m.M[3][2] = -f.Dot(eye);
            return m;
        }

        TMat44 operator*(const TMat44& b) const {
            TMat44 result{};
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    for (int k = 0; k < 4; ++k)
                        result.M[row][col] += M[row][k] * b.M[k][col];
            return result;
        }

        TVec4<T> operator*(const TVec4<T>& v) const {
            return {
                M[0][0]*v.X + M[0][1]*v.Y + M[0][2]*v.Z + M[0][3]*v.W,
                M[1][0]*v.X + M[1][1]*v.Y + M[1][2]*v.Z + M[1][3]*v.W,
                M[2][0]*v.X + M[2][1]*v.Y + M[2][2]*v.Z + M[2][3]*v.W,
                M[3][0]*v.X + M[3][1]*v.Y + M[3][2]*v.Z + M[3][3]*v.W,
            };
        }

        TMat44 GetTransposed() const {
            TMat44 t{};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    t.M[r][c] = M[c][r];
            return t;
        }

        TVec3<T> GetRow3(int i) const { return {M[i][0], M[i][1], M[i][2]}; }

        const T* Data() const { return &M[0][0]; }
    };

    using Mat44 = TMat44<f32>;
} 
