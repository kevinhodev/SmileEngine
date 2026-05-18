#pragma once

#include "Smile/Core/Types.h"
#include <vector>

namespace Smile {
    struct Vertex {
        f32 Position[3];
        f32 Normal[3];
    };

    struct FMesh {
        std::vector<Vertex> Vertices;
        std::vector<u16>    Indices;

        static FMesh CreateCube();
    };
} 
