#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RayTracing/RTTriangle.h"
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

        // Payload de RT por triangulo (v8 do cozido). Vem PRONTO do .smesh; para malha gerada em
        // runtime (primitivas do editor, preview de material, proxy do terreno) fica vazio aqui e
        // quem sobe a geometria o constroi — ver ResolveRTTriangles. Sempre Indices.size()/3
        // entradas quando presente, na MESMA ordem do IB.
        std::vector<FRTTriangle> RTTriangles;

        static FMesh CreateCube();
        static FMesh CreateSphere(u32 Slices = 64, u32 Stacks = 32, f32 Radius = 0.5f);
        // Primitivas do preview de material: plano XZ (normal +Y) e cilindro em Y.
        static FMesh CreatePlane(f32 Size = 1.0f);
        static FMesh CreateCylinder(u32 Slices = 64, f32 Radius = 0.35f, f32 Height = 0.9f);
    };

    // Payload de RT do mesh, gerando em `_Scratch` quando ele NAO veio do cozido. E o ponto unico
    // que faz malha procedural e malha cozida seguirem o mesmo caminho a partir daqui: o
    // FGpuMesh::Upload (primitivas do editor, preview de material) e o FScene::AddMeshesBatch
    // (proxy do terreno) chamam os dois este.
    //
    // Recebe FMesh const de proposito: a cena cozida chega por `const FPreparedCookedScene&`, e
    // mutar o FMesh so para preencher um cache exigiria abrir a constness de todo o caminho de
    // load. O scratch fica com o CHAMADOR, que ja tem escopo para isso.
    inline const std::vector<FRTTriangle>& ResolveRTTriangles(const FMesh& _Mesh,
                                                              std::vector<FRTTriangle>& _Scratch) {
        if (!_Mesh.RTTriangles.empty()) return _Mesh.RTTriangles;
        if (!_Scratch.empty()) return _Scratch;           // ja gerado nesta chamada
        if (_Mesh.Indices.empty() || _Mesh.Vertices.empty()) return _Scratch; // vazio
        BuildRTTriangles(_Mesh.Vertices.data(), static_cast<u32>(_Mesh.Vertices.size()),
                         _Mesh.Indices.data(), static_cast<u32>(_Mesh.Indices.size()), _Scratch);
        return _Scratch;
    }
} 
