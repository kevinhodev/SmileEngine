#pragma once

#include "Smile/Core/Types.h"
#include <vector>

namespace Smile {

struct Vertex {
    f32 pos[3];
    f32 normal[3];
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<u16>    indices;

    static Mesh CreateCube();
};

} // namespace Smile
