#include "Smile/Graphics/Mesh.h"
#include <cmath>

namespace Smile {
    FMesh FMesh::CreateCube() {
        constexpr f32 S = 0.5f;

        constexpr f32 normFront[3]  = { 0.0f,  0.0f, -1.0f };
        constexpr f32 normBack[3]   = { 0.0f,  0.0f,  1.0f };
        constexpr f32 normBottom[3] = { 0.0f, -1.0f,  0.0f };
        constexpr f32 normTop[3]    = { 0.0f,  1.0f,  0.0f };
        constexpr f32 normLeft[3]   = {-1.0f,  0.0f,  0.0f };
        constexpr f32 normRight[3]  = { 1.0f,  0.0f,  0.0f };

        FMesh Mesh;
        Mesh.Vertices = {
            {{-S,  S, -S}, {normFront[0],  normFront[1],  normFront[2]},  {0.0f, 0.0f}},
            {{ S,  S, -S}, {normFront[0],  normFront[1],  normFront[2]},  {1.0f, 0.0f}},
            {{ S, -S, -S}, {normFront[0],  normFront[1],  normFront[2]},  {1.0f, 1.0f}},
            {{-S, -S, -S}, {normFront[0],  normFront[1],  normFront[2]},  {0.0f, 1.0f}},

            {{ S,  S,  S}, {normBack[0],   normBack[1],   normBack[2]},   {0.0f, 0.0f}},
            {{-S,  S,  S}, {normBack[0],   normBack[1],   normBack[2]},   {1.0f, 0.0f}},
            {{-S, -S,  S}, {normBack[0],   normBack[1],   normBack[2]},   {1.0f, 1.0f}},
            {{ S, -S,  S}, {normBack[0],   normBack[1],   normBack[2]},   {0.0f, 1.0f}},

            {{-S,  S,  S}, {normTop[0],    normTop[1],    normTop[2]},    {0.0f, 0.0f}},
            {{ S,  S,  S}, {normTop[0],    normTop[1],    normTop[2]},    {1.0f, 0.0f}},
            {{ S,  S, -S}, {normTop[0],    normTop[1],    normTop[2]},    {1.0f, 1.0f}},
            {{-S,  S, -S}, {normTop[0],    normTop[1],    normTop[2]},    {0.0f, 1.0f}},

            {{-S, -S, -S}, {normBottom[0], normBottom[1], normBottom[2]}, {0.0f, 0.0f}},
            {{ S, -S, -S}, {normBottom[0], normBottom[1], normBottom[2]}, {1.0f, 0.0f}},
            {{ S, -S,  S}, {normBottom[0], normBottom[1], normBottom[2]}, {1.0f, 1.0f}},
            {{-S, -S,  S}, {normBottom[0], normBottom[1], normBottom[2]}, {0.0f, 1.0f}},

            {{-S,  S,  S}, {normLeft[0],   normLeft[1],   normLeft[2]},   {0.0f, 0.0f}},
            {{-S,  S, -S}, {normLeft[0],   normLeft[1],   normLeft[2]},   {1.0f, 0.0f}},
            {{-S, -S, -S}, {normLeft[0],   normLeft[1],   normLeft[2]},   {1.0f, 1.0f}},
            {{-S, -S,  S}, {normLeft[0],   normLeft[1],   normLeft[2]},   {0.0f, 1.0f}},

            {{ S,  S, -S}, {normRight[0],  normRight[1],  normRight[2]},  {0.0f, 0.0f}},
            {{ S,  S,  S}, {normRight[0],  normRight[1],  normRight[2]},  {1.0f, 0.0f}},
            {{ S, -S,  S}, {normRight[0],  normRight[1],  normRight[2]},  {1.0f, 1.0f}},
            {{ S, -S, -S}, {normRight[0],  normRight[1],  normRight[2]},  {0.0f, 1.0f}},
        };
        Mesh.Indices = {
             0,  1,  2,   0,  2,  3,  
             4,  5,  6,   4,  6,  7,  
             8,  9, 10,   8, 10, 11,  
            12, 13, 14,  12, 14, 15,  
            16, 17, 18,  16, 18, 19,  
            20, 21, 22,  20, 22, 23,  
        };
        return Mesh;
    }

    FMesh FMesh::CreateSphere(u32 Slices, u32 Stacks, f32 Radius) {
        constexpr f32 PI = 3.14159265358979323846f;

        FMesh Mesh;
        Mesh.Vertices.reserve((Stacks + 1) * (Slices + 1));

        for (u32 i = 0; i <= Stacks; ++i) {
            const f32 v   = static_cast<f32>(i) / static_cast<f32>(Stacks);
            const f32 lat = v * PI;            
            const f32 y   = std::cos(lat);
            const f32 r   = std::sin(lat);
            for (u32 j = 0; j <= Slices; ++j) {
                const f32 u   = static_cast<f32>(j) / static_cast<f32>(Slices);
                const f32 lon = u * 2.0f * PI; 
                const f32 nx  = r * std::cos(lon);
                const f32 ny  = y;
                const f32 nz  = r * std::sin(lon);

                Vertex Vtx;
                Vtx.Position[0] = nx * Radius;
                Vtx.Position[1] = ny * Radius;
                Vtx.Position[2] = nz * Radius;
                Vtx.Normal[0]   = nx;
                Vtx.Normal[1]   = ny;
                Vtx.Normal[2]   = nz;
                Vtx.TexCoord[0] = u;
                Vtx.TexCoord[1] = v;
                Mesh.Vertices.push_back(Vtx);
            }
        }

        const u32 Cols = Slices + 1;
        Mesh.Indices.reserve(Stacks * Slices * 6);
        for (u32 i = 0; i < Stacks; ++i) {
            for (u32 j = 0; j < Slices; ++j) {
                const u32 a = i       * Cols + j;
                const u32 b = (i + 1) * Cols + j;
                Mesh.Indices.push_back(a);
                Mesh.Indices.push_back(a + 1);
                Mesh.Indices.push_back(b);
                Mesh.Indices.push_back(a + 1);
                Mesh.Indices.push_back(b + 1);
                Mesh.Indices.push_back(b);
            }
        }
        return Mesh;
    }
}
