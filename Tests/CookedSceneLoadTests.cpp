// Harness de CARGA da cena cozida, independente de OMM.
//
// Existe porque o SmileOpacityMicromapBakeTests so validava index buffer de malhas
// ALPHA-TESTED e PULAVA silenciosamente renderaveis com MeshIndex/MaterialIndex invalido.
// Nas cinco cenas sem material masked (Sponza x2, BEEPLE x3) ele nao validava indice nenhum:
// provava apenas que LoadCookedSceneData devolveu sucesso. Este teste fecha a garantia —
// percorre TODAS as referencias e TODOS os index buffers.
//
// Ver Docs/GPU-DRIVEN-PLAN.md, Fase 0 (gate "todas as cenas carregam com v8").

#include "Smile/Scene/SceneLoader.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace {
    int Failures = 0;

    void Fail(const char* _What, const char* _Fmt, ...) {
        va_list Args;
        va_start(Args, _Fmt);
        std::fprintf(stderr, "[FALHA] %s: ", _What);
        std::vfprintf(stderr, _Fmt, Args);
        std::fprintf(stderr, "\n");
        va_end(Args);
        ++Failures;
    }

    bool IsFiniteVec3(const float* _V) {
        return std::isfinite(_V[0]) && std::isfinite(_V[1]) && std::isfinite(_V[2]);
    }
}

int main(int argc, char** argv) {
    using namespace Smile;
    if (argc != 2) {
        std::fprintf(stderr, "uso: SmileCookedSceneLoadTests <cena.sscene>\n");
        return 2;
    }

    const std::filesystem::path ScenePath(argv[1]);
    const auto Scene = LoadCookedSceneData(ScenePath.wstring());
    if (!Scene) {
        std::fprintf(stderr, "[FALHA] LoadCookedSceneData devolveu nulo para %s\n", argv[1]);
        return 3;
    }

    // --- Cabecalhos: magic e versao. O loader ja rejeita versao diferente, mas checar aqui
    // deixa a falha legivel e cobre o caso de o contrato mudar sem o teste perceber.
    if (Scene->SceneHeader.Magic != kSSceneMagic)
        Fail("sscene", "magic 0x%08X != 0x%08X", Scene->SceneHeader.Magic, kSSceneMagic);
    if (Scene->MeshHeader.Magic != kSMeshMagic)
        Fail("smesh", "magic 0x%08X != 0x%08X", Scene->MeshHeader.Magic, kSMeshMagic);
    if (Scene->SceneHeader.Version != kCookedVersion)
        Fail("sscene", "versao %u != %u", Scene->SceneHeader.Version, kCookedVersion);
    if (Scene->MeshHeader.Version != kCookedVersion)
        Fail("smesh", "versao %u != %u", Scene->MeshHeader.Version, kCookedVersion);

    // --- Coerencia entre cabecalho e vetores materializados.
    if (Scene->Materials.size() != Scene->SceneHeader.MaterialCount)
        Fail("materiais", "%zu materializados, cabecalho diz %u",
             Scene->Materials.size(), Scene->SceneHeader.MaterialCount);
    if (Scene->Renderables.size() != Scene->SceneHeader.RenderableCount)
        Fail("renderaveis", "%zu materializados, cabecalho diz %u",
             Scene->Renderables.size(), Scene->SceneHeader.RenderableCount);
    if (Scene->Meshes.size() != Scene->MeshHeader.MeshCount)
        Fail("meshes", "%zu materializadas, cabecalho diz %u",
             Scene->Meshes.size(), Scene->MeshHeader.MeshCount);
    if (Scene->MeshEntries.size() != Scene->Meshes.size())
        Fail("meshes", "%zu entries para %zu malhas", Scene->MeshEntries.size(), Scene->Meshes.size());

    // --- TODAS as referencias dos renderaveis. Ao contrario do harness de OMM, indice invalido
    // aqui e FALHA, nao motivo para pular.
    const size_t MeshCount = Scene->Meshes.size();
    const size_t MatCount  = Scene->Materials.size();
    const size_t RndCount  = Scene->Renderables.size();
    for (size_t I = 0; I < RndCount; ++I) {
        const SSceneRenderable& R = Scene->Renderables[I];
        if (R.MeshIndex >= MeshCount)
            Fail("renderavel", "%zu aponta para mesh %u (>= %zu)", I, R.MeshIndex, MeshCount);
        if (R.MaterialIndex != kNoMaterial && R.MaterialIndex >= MatCount)
            Fail("renderavel", "%zu aponta para material %u (>= %zu)", I, R.MaterialIndex, MatCount);
        // ParentIndex e o indice de OUTRO renderavel; -1 = raiz. Auto-referencia formaria ciclo.
        if (R.ParentIndex != -1) {
            if (R.ParentIndex < 0 || static_cast<size_t>(R.ParentIndex) >= RndCount)
                Fail("renderavel", "%zu tem ParentIndex %d fora de [0,%zu)", I, R.ParentIndex, RndCount);
            else if (R.ParentIndex == static_cast<i32>(I))
                Fail("renderavel", "%zu e pai de si mesmo", I);
        }
        if (!IsFiniteVec3(R.Position) || !IsFiniteVec3(R.RotationEuler) || !IsFiniteVec3(R.Scale))
            Fail("renderavel", "%zu tem transform nao-finito", I);
    }

    // --- TODOS os index buffers, nao so os de malha alpha-tested.
    size_t TotalTris = 0, TotalVerts = 0;
    for (size_t M = 0; M < MeshCount; ++M) {
        const FMesh& Mesh = Scene->Meshes[M];
        TotalVerts += Mesh.Vertices.size();
        TotalTris  += Mesh.Indices.size() / 3;

        if (Mesh.Indices.size() % 3 != 0)
            Fail("mesh", "%zu tem %zu indices (nao multiplo de 3)", M, Mesh.Indices.size());

        const size_t VertCount = Mesh.Vertices.size();
        for (size_t K = 0; K < Mesh.Indices.size(); ++K) {
            if (Mesh.Indices[K] >= VertCount) {
                Fail("mesh", "%zu: indice[%zu] = %u >= %zu vertices",
                     M, K, Mesh.Indices[K], VertCount);
                break; // um relato por malha ja identifica o arquivo; nao inundar a saida
            }
        }

        // Contrato da v8: o payload de RT e posicional, um registro por PrimitiveIndex.
        const size_t RtCount = Mesh.RTTriangles.size();
        if (RtCount != 0 && RtCount != Mesh.Indices.size() / 3)
            Fail("mesh", "%zu: %zu registros de RT para %zu triangulos",
                 M, RtCount, Mesh.Indices.size() / 3);

        // A caixa do cozido e LOCAL desde a v7 e alimenta culling e HiZ; degenerada passa
        // despercebida no raster e some no culling.
        const SMeshEntry& E = Scene->MeshEntries[M];
        if (!IsFiniteVec3(E.AABBMin) || !IsFiniteVec3(E.AABBMax))
            Fail("mesh", "%zu tem AABB nao-finita", M);
        else if (E.AABBMin[0] > E.AABBMax[0] || E.AABBMin[1] > E.AABBMax[1] ||
                 E.AABBMin[2] > E.AABBMax[2])
            Fail("mesh", "%zu tem AABB invertida", M);
    }

    std::printf("[LOAD] %s\n", ScenePath.filename().string().c_str());
    std::printf("[LOAD] v%u | renderaveis=%zu meshes=%zu materiais=%zu | verts=%zu tris=%zu\n",
                Scene->SceneHeader.Version, RndCount, MeshCount, MatCount, TotalVerts, TotalTris);
    std::printf("[LOAD] texturas=%zu | read=%.1fms decode=%.1fms mesh=%.1fms prepare=%.1fms\n",
                Scene->TextureData.size(), Scene->ReadMs, Scene->DecodeMs,
                Scene->MeshMs, Scene->PrepareMs);

    if (Failures > 0) {
        std::fprintf(stderr, "[LOAD] %d falha(s)\n", Failures);
        return 4;
    }
    std::printf("[LOAD] ok - todas as referencias e index buffers validados\n");
    return 0;
}
