#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Scene/SceneLoader.h"
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

// As quatro ENTRADAS do bake tem de invalidar o micro-map. Antes da Fase 2 o guarda comparava
// duas (alpha e cutoff): trocar a textura de albedo de um material alpha-tested passava por baixo
// dele e deixava o OMM classificando microtriangulos pelo alpha da textura ANTIGA — folhagem com
// recorte errado, sem excecao e sem erro.
//
// Roda antes da checagem de argumentos porque nao depende de cena nenhuma: e CPU pura sobre a
// identidade das entradas.
static int TestarChaveDeEntradas() {
    using namespace Smile;
    int Falhas = 0;
    auto Check = [&](bool Cond, const char* Msg) {
        if (!Cond) { ++Falhas; std::fprintf(stderr, "  FAIL: %s\n", Msg); }
    };

    FGpuMesh MalhaA, MalhaB;
    FTexture TexturaA, TexturaB;
    FMaterial Material;
    Material.Constants.AlphaTest            = 1;
    Material.Constants.AlphaCutoff          = 0.5f;
    Material.Constants.BaseColorFactor.W    = 1.0f;
    Material.Albedo                         = &TexturaA;

    FOpacityMicromapData Omm;
    Omm.BakedInputs = FOmmInputKey::From(&MalhaA, &Material);
    Check(Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "entradas intactas: o micro-map continua valendo");

    // 1) textura de alpha — o furo real que existia.
    Material.Albedo = &TexturaB;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "trocar a textura de albedo invalida o micro-map");
    Material.Albedo = &TexturaA;

    // 2) fator de alpha do material.
    Material.Constants.BaseColorFactor.W = 0.5f;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "mudar o alpha do BaseColor invalida o micro-map");
    Material.Constants.BaseColorFactor.W = 1.0f;

    // 3) cutoff.
    Material.Constants.AlphaCutoff = 0.25f;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "mudar o cutoff invalida o micro-map");
    Material.Constants.AlphaCutoff = 0.5f;

    // 4) o proprio modo: desligar o alpha test troca o BLAS exigido pelo opaco.
    Material.Constants.AlphaTest = 0;
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "desligar o alpha test invalida o micro-map");
    Material.Constants.AlphaTest = 1;

    // 5) a malha.
    Check(!Omm.MatchesInputs(FOmmInputKey::From(&MalhaB, &Material)),
          "outra malha nao pode reusar o micro-map");

    // Um micro-map nunca carimbado nao casa com nada: caminho de criacao novo falha alto.
    FOpacityMicromapData SemCarimbo;
    Check(!SemCarimbo.MatchesInputs(FOmmInputKey::From(&MalhaA, &Material)),
          "micro-map sem carimbo de entradas e recusado");
    // E chave viva vazia tambem nao casa com um bake real.
    Check(!Omm.MatchesInputs(FOmmInputKey::From(nullptr, &Material)),
          "chave viva incompleta nao valida um bake");

    if (Falhas == 0) std::printf("[OMM TEST] chave de entradas do bake: 8 casos OK\n");
    return Falhas;
}

int main(int argc, char** argv) {
    using namespace Smile;
    if (TestarChaveDeEntradas() != 0) return 5;
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
