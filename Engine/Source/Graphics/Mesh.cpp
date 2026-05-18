#include "Smile/Graphics/Mesh.h"

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
            // Front (Z = -S)
            {{-S,  S, -S}, {normFront[0],  normFront[1],  normFront[2]}},
            {{ S,  S, -S}, {normFront[0],  normFront[1],  normFront[2]}},
            {{ S, -S, -S}, {normFront[0],  normFront[1],  normFront[2]}},
            {{-S, -S, -S}, {normFront[0],  normFront[1],  normFront[2]}},
            // Back (Z = +S)
            {{ S,  S,  S}, {normBack[0],   normBack[1],   normBack[2]}},
            {{-S,  S,  S}, {normBack[0],   normBack[1],   normBack[2]}},
            {{-S, -S,  S}, {normBack[0],   normBack[1],   normBack[2]}},
            {{ S, -S,  S}, {normBack[0],   normBack[1],   normBack[2]}},
            // Top (Y = +S)
            {{-S,  S,  S}, {normTop[0],    normTop[1],    normTop[2]}},
            {{ S,  S,  S}, {normTop[0],    normTop[1],    normTop[2]}},
            {{ S,  S, -S}, {normTop[0],    normTop[1],    normTop[2]}},
            {{-S,  S, -S}, {normTop[0],    normTop[1],    normTop[2]}},
            // Bottom (Y = -S)
            {{-S, -S, -S}, {normBottom[0], normBottom[1], normBottom[2]}},
            {{ S, -S, -S}, {normBottom[0], normBottom[1], normBottom[2]}},
            {{ S, -S,  S}, {normBottom[0], normBottom[1], normBottom[2]}},
            {{-S, -S,  S}, {normBottom[0], normBottom[1], normBottom[2]}},
            // Left (X = -S)
            {{-S,  S,  S}, {normLeft[0],   normLeft[1],   normLeft[2]}},
            {{-S,  S, -S}, {normLeft[0],   normLeft[1],   normLeft[2]}},
            {{-S, -S, -S}, {normLeft[0],   normLeft[1],   normLeft[2]}},
            {{-S, -S,  S}, {normLeft[0],   normLeft[1],   normLeft[2]}},
            // Right (X = +S)
            {{ S,  S, -S}, {normRight[0],  normRight[1],  normRight[2]}},
            {{ S,  S,  S}, {normRight[0],  normRight[1],  normRight[2]}},
            {{ S, -S,  S}, {normRight[0],  normRight[1],  normRight[2]}},
            {{ S, -S, -S}, {normRight[0],  normRight[1],  normRight[2]}},
        };
        Mesh.Indices = {
             0,  1,  2,   0,  2,  3,  // Front
             4,  5,  6,   4,  6,  7,  // Back
             8,  9, 10,   8, 10, 11,  // Top
            12, 13, 14,  12, 14, 15,  // Bottom
            16, 17, 18,  16, 18, 19,  // Left
            20, 21, 22,  20, 22, 23,  // Right
        };
        return Mesh;
    }
} 
