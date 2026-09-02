// Gerador de cena SINTETICA para a regua de escala da Fase 0 (Docs/GPU-DRIVEN-PLAN.md).
//
// Por que gerar um .smesh/.sscene em vez de povoar a cena em runtime: assim a cena sintetica
// entra pelo MESMO caminho de load que as reais (SceneLoader -> FScene::AddMeshesBatch -> TLAS
// -> telemetria), e tudo que ja existe continua valendo sem caso especial — os testes de carga,
// o painel e o profile_snapshot do MCP nao sabem que a cena e sintetica. Um povoamento por
// runtime mediria um caminho que nenhuma cena real percorre.
//
// O que a cena precisa ter para a medida significar algo:
//
//   - GEOMETRIA COMPARTILHADA. N instancias sobre poucas malhas e o regime que interessa: e o
//     que o Emerald Square ja faz (2479 renderaveis, 281 malhas) e o que o GPU-driven explora.
//     Gerar N malhas unicas mediria custo de VRAM e de BLAS, nao de submissao.
//   - VARIACAO DE MATERIAL CONTROLAVEL. O numero de materiais e o que decide quantos binds
//     sobram depois do cache de submissao, e depois do bindless sera o que decide o numero de
//     lotes. Com material unico o teste seria otimista; com um por instancia, pessimista.
//   - FRACAO MASCARADA CONTROLAVEL. Alpha-test e two-sided partem o z-prepass em dois lacos e
//     tem PSO proprio, entao a distribuicao entre eles muda o numero de trocas de estado.

#include "Smile/Scene/CookedFormat.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/RayTracing/RTTriangle.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
    using Smile::Vertex;

    // Grade plana TxT quads, dobrada num "telhado" para a malha nao ser degenerada no eixo Y
    // (AABB achatada passaria despercebida no culling e no HiZ, que e justamente o que o teste
    // de escala exercita). 2*T*T triangulos.
    void BuildGridMesh(uint32_t _Tess, std::vector<Vertex>& _Vertices,
                       std::vector<uint32_t>& _Indices,
                       float _AabbMin[3], float _AabbMax[3]) {
        const uint32_t N = _Tess + 1u;
        _Vertices.clear();
        _Vertices.reserve(static_cast<size_t>(N) * N);
        _Indices.clear();
        _Indices.reserve(static_cast<size_t>(_Tess) * _Tess * 6u);

        for (int c = 0; c < 3; ++c) { _AabbMin[c] = 1e30f; _AabbMax[c] = -1e30f; }

        for (uint32_t z = 0; z < N; ++z) {
            for (uint32_t x = 0; x < N; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(_Tess);
                const float v = static_cast<float>(z) / static_cast<float>(_Tess);
                // Altura em telhado: sobe ate o meio e desce. Da volume sem custo de seno.
                const float h = 0.5f * (1.0f - std::fabs(2.0f * u - 1.0f))
                              + 0.5f * (1.0f - std::fabs(2.0f * v - 1.0f));

                Vertex Vtx{};
                Vtx.Position[0] = u - 0.5f;
                Vtx.Position[1] = h * 0.5f;
                Vtx.Position[2] = v - 0.5f;
                // Normal aproximada por diferenca do telhado; nao precisa ser exata, precisa ser
                // finita e variada (normal constante deixaria o G-buffer sem trabalho real).
                const float dx = (u < 0.5f ? 1.0f : -1.0f);
                const float dz = (v < 0.5f ? 1.0f : -1.0f);
                const float nl = std::sqrt(dx * dx + 4.0f + dz * dz);
                Vtx.Normal[0] = -dx * 0.5f / nl;
                Vtx.Normal[1] = 2.0f / nl;
                Vtx.Normal[2] = -dz * 0.5f / nl;
                Vtx.TexCoord[0] = u;
                Vtx.TexCoord[1] = v;

                for (int c = 0; c < 3; ++c) {
                    if (Vtx.Position[c] < _AabbMin[c]) _AabbMin[c] = Vtx.Position[c];
                    if (Vtx.Position[c] > _AabbMax[c]) _AabbMax[c] = Vtx.Position[c];
                }
                _Vertices.push_back(Vtx);
            }
        }

        for (uint32_t z = 0; z < _Tess; ++z) {
            for (uint32_t x = 0; x < _Tess; ++x) {
                const uint32_t I0 = z * N + x;
                const uint32_t I1 = I0 + 1u;
                const uint32_t I2 = I0 + N;
                const uint32_t I3 = I2 + 1u;
                // Winding igual ao que o cooker de FBX produz (LH, CW visto de +Y).
                _Indices.push_back(I0); _Indices.push_back(I2); _Indices.push_back(I1);
                _Indices.push_back(I1); _Indices.push_back(I2); _Indices.push_back(I3);
            }
        }
    }
}

// Declarado em main.cpp; mantido fora de header proprio porque e o unico ponto de contato.
int GenerateSyntheticScene(const fs::path& _OutBase, uint32_t _Instances, uint32_t _MeshCount,
                           uint32_t _MaterialCount, uint32_t _MaskedPercent, float _Spacing,
                           uint32_t _BaseTess) {
    if (_Instances == 0 || _MeshCount == 0 || _MaterialCount == 0) {
        std::printf("[Cooker] ERRO: instances/meshes/materials precisam ser > 0\n");
        return 2;
    }

    std::vector<Smile::SMeshEntry>      Entries;
    std::vector<uint8_t>                Geo;
    std::vector<Smile::SSceneMaterial>  Materials;
    std::vector<Smile::SSceneRenderable> Renderables;
    std::vector<Smile::FRTTriangle>     RtTris;

    // --- Malhas. Tesselacao cresce com o indice: a distribuicao de triangulos por draw importa
    // para o custo de VS, e malhas identicas dariam um perfil que nenhuma cena real tem.
    size_t TotalTris = 0;
    std::vector<Vertex>   Vertices;
    std::vector<uint32_t> Indices;
    for (uint32_t M = 0; M < _MeshCount; ++M) {
        const uint32_t Tess = _BaseTess + M * 2u;
        float MinP[3], MaxP[3];
        BuildGridMesh(Tess, Vertices, Indices, MinP, MaxP);

        Smile::SMeshEntry E{};
        E.VertexCount = static_cast<uint32_t>(Vertices.size());
        E.IndexCount  = static_cast<uint32_t>(Indices.size());
        for (int c = 0; c < 3; ++c) { E.AABBMin[c] = MinP[c]; E.AABBMax[c] = MaxP[c]; }

        E.VertexOffset = Geo.size();
        Geo.insert(Geo.end(), reinterpret_cast<uint8_t*>(Vertices.data()),
                   reinterpret_cast<uint8_t*>(Vertices.data()) + Vertices.size() * sizeof(Vertex));
        E.IndexOffset = Geo.size();
        Geo.insert(Geo.end(), reinterpret_cast<uint8_t*>(Indices.data()),
                   reinterpret_cast<uint8_t*>(Indices.data()) + Indices.size() * sizeof(uint32_t));

        // Mesmo contrato posicional da v8: um registro por PrimitiveIndex, gerado DEPOIS do
        // winding final (aqui o winding ja e o definitivo, nao ha weld nem inversao depois).
        Smile::BuildRTTriangles(Vertices.data(), static_cast<uint32_t>(Vertices.size()),
                                Indices.data(), static_cast<uint32_t>(Indices.size()), RtTris);
        E.RTTriangleOffset = Geo.size();
        E.RTTriangleCount  = static_cast<uint32_t>(RtTris.size());
        Geo.insert(Geo.end(), reinterpret_cast<uint8_t*>(RtTris.data()),
                   reinterpret_cast<uint8_t*>(RtTris.data()) + RtTris.size() * sizeof(Smile::FRTTriangle));

        TotalTris += RtTris.size();
        Entries.push_back(E);
    }

    // --- Materiais. Sem textura de proposito: o teste mede submissao e culling, e carregar
    // milhares de texturas mediria o streaming, que e outro eixo e tem regua propria.
    const uint32_t MaskedCount =
        (_MaterialCount * (_MaskedPercent > 100u ? 100u : _MaskedPercent) + 99u) / 100u;
    for (uint32_t K = 0; K < _MaterialCount; ++K) {
        Smile::SSceneMaterial Mat{};
        std::snprintf(Mat.Name, Smile::kCookedMaxName, "synthetic_mat_%u", K);
        const float T = _MaterialCount > 1
                      ? static_cast<float>(K) / static_cast<float>(_MaterialCount - 1u)
                      : 0.0f;
        Mat.BaseColorFactor[0] = 0.25f + 0.70f * T;
        Mat.BaseColorFactor[1] = 0.55f;
        Mat.BaseColorFactor[2] = 0.95f - 0.70f * T;
        Mat.BaseColorFactor[3] = 1.0f;
        Mat.MetallicFactor  = (K % 3u == 0u) ? 1.0f : 0.0f;
        Mat.RoughnessFactor = 0.15f + 0.80f * T;
        // Os primeiros K materiais sao mascarados. Sem alpha map, o clip cai no fator de alpha,
        // que fica em 1.0 — o objetivo aqui e exercitar o PSO e o laco separados do prepass,
        // nao recortar pixel.
        if (K < MaskedCount) {
            Mat.AlphaTest   = 1;
            Mat.AlphaCutoff = 0.5f;
            Mat.TwoSided    = 1;
        }
        Materials.push_back(Mat);
    }

    // --- Instancias num cubo aproximado. Distribuicao 3D e nao um tapete plano: com tudo no
    // mesmo plano, o frustum culling aceita ou rejeita quase tudo de uma vez e a fracao visivel
    // deixa de responder ao movimento da camera, que e o eixo que o teste precisa exercitar.
    const uint32_t Side = static_cast<uint32_t>(
        std::ceil(std::cbrt(static_cast<double>(_Instances))));
    Renderables.reserve(_Instances);
    for (uint32_t I = 0; I < _Instances; ++I) {
        const uint32_t X = I % Side;
        const uint32_t Y = (I / Side) % Side;
        const uint32_t Z = I / (Side * Side);

        Smile::SSceneRenderable R{};
        R.MeshIndex     = I % _MeshCount;
        R.MaterialIndex = I % _MaterialCount;
        R.Position[0] = (static_cast<float>(X) - 0.5f * Side) * _Spacing;
        R.Position[1] = (static_cast<float>(Y) - 0.5f * Side) * _Spacing;
        R.Position[2] = (static_cast<float>(Z) - 0.5f * Side) * _Spacing;
        R.RotationEuler[0] = 0.0f;
        R.RotationEuler[1] = static_cast<float>(I % 8u) * 0.7853981634f; // 45 graus por passo
        R.RotationEuler[2] = 0.0f;
        R.Scale[0] = R.Scale[1] = R.Scale[2] = 1.0f;
        R.ParentIndex = -1;
        std::snprintf(R.Name, Smile::kCookedMaxName, "synth_%u", I);
        Renderables.push_back(R);
    }

    const double GeoMB = static_cast<double>(Geo.size()) / (1024.0 * 1024.0);
    const double SceneMB = static_cast<double>(
        sizeof(Smile::SSceneHeader) + Materials.size() * sizeof(Smile::SSceneMaterial) +
        Renderables.size() * sizeof(Smile::SSceneRenderable)) / (1024.0 * 1024.0);
    std::printf("[Cooker] Sintetica: %u instancias | %u malhas | %u materiais (%u mascarados) | "
                "%zu triangulos unicos\n",
                _Instances, _MeshCount, _MaterialCount, MaskedCount, TotalTris);
    std::printf("[Cooker] Instancias por malha: %.1f | grid %ux%ux%u | espacamento %.2f\n",
                static_cast<double>(_Instances) / _MeshCount, Side, Side, Side, _Spacing);
    std::printf("[Cooker] Geo: %.1f MB | Cena: %.1f MB\n", GeoMB, SceneMB);

    {
        fs::path P = _OutBase; P += ".smesh";
        FILE* F = std::fopen(P.string().c_str(), "wb");
        if (!F) { std::printf("[Cooker] ERRO ao abrir %s\n", P.string().c_str()); return 3; }
        Smile::SMeshHeader H{ Smile::kSMeshMagic, Smile::kCookedVersion,
                              static_cast<uint32_t>(Entries.size()), 0 };
        std::fwrite(&H, sizeof(H), 1, F);
        std::fwrite(Entries.data(), sizeof(Smile::SMeshEntry), Entries.size(), F);
        std::fwrite(Geo.data(), 1, Geo.size(), F);
        std::fclose(F);
        std::printf("[Cooker] Escrito: %s\n", P.string().c_str());
    }
    {
        fs::path P = _OutBase; P += ".sscene";
        FILE* F = std::fopen(P.string().c_str(), "wb");
        if (!F) { std::printf("[Cooker] ERRO ao abrir %s\n", P.string().c_str()); return 3; }
        Smile::SSceneHeader H{ Smile::kSSceneMagic, Smile::kCookedVersion,
                               static_cast<uint32_t>(Materials.size()),
                               static_cast<uint32_t>(Renderables.size()) };
        std::fwrite(&H, sizeof(H), 1, F);
        std::fwrite(Materials.data(), sizeof(Smile::SSceneMaterial), Materials.size(), F);
        std::fwrite(Renderables.data(), sizeof(Smile::SSceneRenderable), Renderables.size(), F);
        std::fclose(F);
        std::printf("[Cooker] Escrito: %s\n", P.string().c_str());
    }
    std::printf("[Cooker] OK.\n");
    return 0;
}
