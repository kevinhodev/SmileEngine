#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Scene/SceneLoader.h"
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

int main(int argc, char** argv) {
    using namespace Smile;
    if (argc != 2) {
        std::fprintf(stderr, "uso: SmileOpacityMicromapBakeTests <cena.sscene>\n");
        return 2;
    }

    const auto Imported = LoadCookedSceneData(std::filesystem::path(argv[1]).wstring());
    if (!Imported) {
        std::fprintf(stderr, "falha ao preparar cena: %s\n", argv[1]);
        return 3;
    }

    std::unordered_map<std::string, const FTextureCPUData*> Textures;
    for (size_t I = 0; I < Imported->TexturePaths.size(); ++I)
        Textures[Imported->TexturePaths[I]] = &Imported->TextureData[I];

    std::unordered_set<u64> Seen;
    u32 BakeCount = 0;
    for (u32 I = 0; I < Imported->Renderables.size(); ++I) {
        const SSceneRenderable& R = Imported->Renderables[I];
        if (R.MaterialIndex == kNoMaterial || R.MaterialIndex >= Imported->Materials.size() ||
            R.MeshIndex >= Imported->Meshes.size())
            continue;
        const SSceneMaterial& M = Imported->Materials[R.MaterialIndex];
        if (!M.AlphaTest) continue;

        const u64 Key = (static_cast<u64>(R.MeshIndex) << 32) | R.MaterialIndex;
        if (!Seen.insert(Key).second) continue;
        const FMesh& Mesh = Imported->Meshes[R.MeshIndex];
        for (u32 Index : Mesh.Indices) {
            if (Index >= Mesh.Vertices.size()) {
                std::fprintf(stderr, "mesh %u possui indice %u >= %zu\n",
                             R.MeshIndex, Index, Mesh.Vertices.size());
                return 4;
            }
        }

        const FTextureCPUData* Alpha = nullptr;
        if (M.BaseColor[0]) {
            const auto It = Textures.find(M.BaseColor);
            if (It != Textures.end()) Alpha = It->second;
        }
        std::printf("[OMM TEST] bake=%u renderable=%u mesh=%u material=%u verts=%zu tris=%zu "
                    "tex=%ux%u fmt=%u path=%s\n",
                    BakeCount, I, R.MeshIndex, R.MaterialIndex, Mesh.Vertices.size(),
                    Mesh.Indices.size() / 3, Alpha ? Alpha->Width : 1,
                    Alpha ? Alpha->Height : 1, Alpha ? static_cast<unsigned>(Alpha->Format) : 0,
                    M.BaseColor);
        std::fflush(stdout);

        const auto Omm = FOpacityMicromapBaker::Bake(
            Mesh, Alpha, M.BaseColorFactor[3], M.AlphaCutoff);
        if (!Omm || !Omm->Valid()) {
            std::fprintf(stderr, "bake %u devolveu resultado invalido\n", BakeCount);
            return 5;
        }
        ++BakeCount;
    }

    std::printf("[OMM TEST] %u pares mesh/material masked validados\n", BakeCount);
    return BakeCount ? 0 : 6;
}
