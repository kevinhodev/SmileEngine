#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Scene/Light.h"
#include <memory>
#include <string>
#include <vector>

namespace Smile {
    class FUploadQueue;

    struct FTransform {
        Vec3 Position      = { 0.0f, 0.0f, 0.0f };
        Vec3 RotationEuler = { 0.0f, 0.0f, 0.0f }; 
        Vec3 Scale         = { 1.0f, 1.0f, 1.0f };

        Mat44 Matrix() const;
    };

    struct FRenderable {
        std::string Name;
        FTransform  Transform;
        FGpuMesh*   Mesh     = nullptr;
        FMaterial*  Material = nullptr;
        bool        Visible  = true;

        Vec3        AABBMin  = { -1e9f, -1e9f, -1e9f };
        Vec3        AABBMax  = {  1e9f,  1e9f,  1e9f };
    };

    class FScene {
    public:
        FGpuMesh* AddMesh(ID3D12Device* Device, const FMesh& Mesh);

        std::vector<FGpuMesh*> AddMeshesBatch(ID3D12Device* Device, FUploadQueue& UploadQueue,
                                              const std::vector<FMesh>& Meshes);

        FRenderable& AddRenderable(const FRenderable& Renderable);

        std::vector<FRenderable>&       Renderables()       { return RenderableList; }
        const std::vector<FRenderable>& Renderables() const { return RenderableList; }

        FLight& AddLight(const FLight& Light);

        std::vector<FLight>&       Lights()       { return LightList; }
        const std::vector<FLight>& Lights() const { return LightList; }

        void Clear();

    private:
        std::vector<std::unique_ptr<FGpuMesh>> MeshLibrary;
        std::vector<FRenderable>               RenderableList;
        std::vector<FLight>                    LightList;
    };
}
