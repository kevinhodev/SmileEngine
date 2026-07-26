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
        std::vector<u32>    Indices; 

        static FMesh CreateCube();
        static FMesh CreateSphere(u32 Slices = 64, u32 Stacks = 32, f32 Radius = 0.5f);
        // Primitivas do preview de material: plano XZ (normal +Y) e cilindro em Y.
        static FMesh CreatePlane(f32 Size = 1.0f);
        static FMesh CreateCylinder(u32 Slices = 64, f32 Radius = 0.35f, f32 Height = 0.9f);
    };
} 
