#pragma once

#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Scene/CookedFormat.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Smile {
    // Resultado CPU do loader. Nao possui device, descriptors ou recursos GPU.
    struct FSceneImportResult {
        std::filesystem::path BasePath;
        std::filesystem::path ScenePath;
        std::filesystem::path SceneDir;

        SSceneHeader SceneHeader{};
        SMeshHeader  MeshHeader{};
        std::vector<SSceneMaterial>   Materials;
        std::vector<SSceneRenderable> Renderables;
        std::vector<SMeshEntry>       MeshEntries;
        std::vector<std::string>      TexturePaths;
        std::vector<FTextureCPUData>  TextureData;
        std::vector<FMesh>            Meshes;

        double ReadMs    = 0.0;
        double DecodeMs  = 0.0;
        double MeshMs    = 0.0;
        double PrepareMs = 0.0;
    };

    using FSceneImportResultPtr = std::shared_ptr<FSceneImportResult>;

    FSceneImportResultPtr LoadCookedSceneData(const std::wstring& ScenePath);
}
