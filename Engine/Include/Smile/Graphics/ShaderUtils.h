#pragma once

#include "Smile/Core/Types.h"
#include <string>
#include <vector>

namespace Smile {
    std::vector<u8> LoadShaderBytecode(const std::string& Filename);
}
