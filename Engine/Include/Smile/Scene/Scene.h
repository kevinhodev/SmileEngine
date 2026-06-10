#pragma once

#include "Smile/Math/Math.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/Material.h"
#include <memory>
#include <string>
#include <vector>

namespace Smile {
    class FCommandQueue;

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
        // AABB em espaco de MUNDO p/ frustum culling. Default = caixa enorme => sempre
        // visivel (renderaveis sem AABB setada, ex.: a esfera demo, nunca sao cortados).
        Vec3        AABBMin  = { -1e9f, -1e9f, -1e9f };
        Vec3        AABBMax  = {  1e9f,  1e9f,  1e9f };
    };

    // Cena minima: uma biblioteca de meshes (dona dos buffers GPU) e uma lista de
    // renderaveis. Sem hierarquia/ECS ainda — base para multi-objeto.
    class FScene {
    public:
        // Cria um GpuMesh na biblioteca a partir de dados CPU. Ponteiro estavel
        // (a biblioteca guarda unique_ptr; o vetor cresce sem invalidar os alvos).
        FGpuMesh* AddMesh(ID3D12Device* Device, const FMesh& Mesh);

        // Sobe MUITOS meshes em lote: geometria em DEFAULT heap, agrupando as copias por
        // chunks (orcamento de staging) com 1 sync por chunk — muito mais rapido que um
        // AddMesh por mesh (1 sync cada). Retorna os ponteiros na ordem de entrada.
        std::vector<FGpuMesh*> AddMeshesBatch(ID3D12Device* Device, FCommandQueue& Queue,
                                              const std::vector<FMesh>& Meshes);

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
