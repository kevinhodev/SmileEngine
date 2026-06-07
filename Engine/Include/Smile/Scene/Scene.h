#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/Material.h"
#include <memory>
#include <string>
#include <vector>

namespace Smile {
    // Transform TRS simples. A matriz de mundo eh Scale * Rotation * Translation
    // (convencao de vetor-linha: world = v * S * R * T). Rotacao em radianos,
    // aplicada na ordem X * Y * Z.
    struct FTransform {
        Vec3 Position      = { 0.0f, 0.0f, 0.0f };
        Vec3 RotationEuler = { 0.0f, 0.0f, 0.0f }; // radianos
        Vec3 Scale         = { 1.0f, 1.0f, 1.0f };

        Mat44 Matrix() const;
    };

    // Um objeto desenhavel: transform + mesh (ref na biblioteca da cena) + material.
    // Material nulo => o Renderer usa o material ativo/default.
    struct FRenderable {
        std::string Name;
        FTransform  Transform;
        FGpuMesh*   Mesh     = nullptr;
        FMaterial*  Material = nullptr;
        bool        Visible  = true;
    };

    // Cena minima: uma biblioteca de meshes (dona dos buffers GPU) e uma lista de
    // renderaveis. Sem hierarquia/ECS ainda — base para multi-objeto.
    class FScene {
    public:
        // Cria um GpuMesh na biblioteca a partir de dados CPU. Ponteiro estavel
        // (a biblioteca guarda unique_ptr; o vetor cresce sem invalidar os alvos).
        FGpuMesh* AddMesh(ID3D12Device* Device, const FMesh& Mesh);

        // Adiciona um renderavel e devolve a referencia para ajustes posteriores.
        FRenderable& AddRenderable(const FRenderable& Renderable);

        std::vector<FRenderable>&       Renderables()       { return RenderableList; }
        const std::vector<FRenderable>& Renderables() const { return RenderableList; }

        void Clear();

    private:
        std::vector<std::unique_ptr<FGpuMesh>> MeshLibrary;
        std::vector<FRenderable>               RenderableList;
    };
}
