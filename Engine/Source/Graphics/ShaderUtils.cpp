#include "Smile/Graphics/ShaderUtils.h"
#include "Smile/Core/Logger.h"
#include <fstream>
#include <stdexcept>

#ifndef SMILE_SHADER_DIR
#error "SMILE_SHADER_DIR nao definido. Verifique o CMake."
#endif

namespace Smile {
    std::vector<u8> LoadShaderBytecode(const std::string& _Filename) {
        const std::string FullPath = std::string(SMILE_SHADER_DIR) + "/" + _Filename;
        std::ifstream File(FullPath, std::ios::binary | std::ios::ate);
        if (!File) {
            LogError("Falha ao abrir shader: " + FullPath);
            throw std::runtime_error("Shader nao encontrado: " + FullPath);
        }
        const auto Size = static_cast<size_t>(File.tellg());
        std::vector<u8> Data(Size);
        File.seekg(0);
        File.read(reinterpret_cast<char*>(Data.data()), Size);
        return Data;
    }
}
