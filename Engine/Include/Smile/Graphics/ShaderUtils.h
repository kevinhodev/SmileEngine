#pragma once

#include "Smile/Core/Types.h"
#include <string>
#include <vector>

namespace Smile {
    // Lê o bytecode de um shader compilado (.cso) a partir de SMILE_SHADER_DIR.
    // Centraliza o helper que estava duplicado em cada subsistema (Pipeline/Atmosphere/
    // Fog/Skybox/Clouds/Water/PostProcess/Compute/...). Lança std::runtime_error se o
    // arquivo não puder ser aberto.
    std::vector<u8> LoadShaderBytecode(const std::string& Filename);
}
