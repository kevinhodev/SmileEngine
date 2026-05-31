#pragma once

#include "Smile/Core/Types.h"
#include <vector>

namespace Smile {
    struct Vertex {
        f32 Position[3];
        f32 Normal[3];
        f32 TexCoord[2];
    };

    struct FMesh {
        std::vector<Vertex> Vertices;
        std::vector<u16>    Indices;

        static FMesh CreateCube();

        // UV sphere centered at origin. Default radius 0.5 matches the cube's extent.
        static FMesh CreateSphere(u32 Slices = 64, u32 Stacks = 32, f32 Radius = 0.5f);
    };
} 
