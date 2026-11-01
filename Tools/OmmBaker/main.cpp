// SmileOmmBaker — bake OFFLINE dos Opacity Micro-Maps de uma cena cozida, para o sidecar
// <cena>.somm que o load consome em vez de bakear.
//
// Por que e uma ferramenta propria, e nao um passo do SmileCooker: o Cooker nao linka a engine (nem
// OMM SDK, nem DirectXTex, nem o loader de textura) — ele so referencia os DDS do disco. O bake tem
// de enxergar o alpha EXATAMENTE como o runtime o enxerga: o BC decodificado dos bytes que sobem
// para a GPU, as UVs do .smesh pos-weld. Por isso esta ferramenta carrega a cena pelo MESMO
// LoadCookedSceneData do runtime e enumera os pares pela MESMA CollectOmmBakeJobs do import.
//
// Sem teto de workload: e a razao de existir. No BistroExterior o teto de runtime deixava 113 k
// triangulos mascarados sem micro-map, em malhas 4x a 34x acima dele.
//
// Incremental por padrao: pares cujo bake no sidecar existente ainda bate com a cena sao
// reaproveitados. --force refaz todos.

#include "Smile/Graphics/RayTracing/OmmBakeCache.h"
#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Scene/SceneLoader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    using namespace Smile;
    using FClock = std::chrono::steady_clock;
    double MsSince(FClock::time_point T) {
        return std::chrono::duration<double, std::milli>(FClock::now() - T).count();
    }

    // O CUSTO DO REBAKE DO EDITOR, por material. Um commit de edicao num material mascarado
    // (cutoff, alpha, troca de albedo) rebakeia TODOS os pares dele pelo caminho de runtime:
    // Renderer::BakeOpacityMicromapFor, que rele a geometria do .smesh, rele a textura do disco
    // (conferindo o hash) e bakeia com o teto de runtime — com a render thread esperando. Este
    // modo repete exatamente essas tres etapas, pelo mesmo FOmmBakeInputStore, e cronometra cada
    // uma. Os pares sao do material com mais triangulos primeiro.
    int BenchRebake(const FSceneImportResult& _Scene, const std::vector<FOmmBakeJob>& _Jobs,
                    u32 _MaxMaterials) {
        // Chaves de identidade do store: objetos vazios, como no teste — nada neles e lido.
        std::vector<FGpuMesh> MeshKeys(_Scene.Meshes.size());
        std::vector<FTexture> TextureKeys(_Scene.TextureData.size());
        std::unordered_map<const FTextureCPUData*, size_t> TextureIndex;
        for (size_t I = 0; I < _Scene.TextureData.size(); ++I)
            TextureIndex[&_Scene.TextureData[I]] = I;

        FOmmBakeInputStore Store;
        const std::wstring MeshFile =
            (std::filesystem::path(_Scene.BasePath) += L".smesh").wstring();
        std::map<u32, std::vector<const FOmmBakeJob*>> ByMaterial;
        for (const FOmmBakeJob& J : _Jobs) {
            Store.RetainMeshSource(&MeshKeys[J.Key.MeshIndex], MeshFile, J.Key.MeshIndex,
                                   J.Key.Geometry);
            if (J.AlphaTexture) {
                const size_t T = TextureIndex.at(J.AlphaTexture);
                Store.RetainTexturePath(
                    &TextureKeys[T],
                    (_Scene.SceneDir / std::filesystem::path(_Scene.TexturePaths[T])).wstring(),
                    *J.AlphaTexture);
            }
            ByMaterial[J.Key.MaterialIndex].push_back(&J);
        }

        std::vector<std::pair<u32, u64>> Order;
        for (const auto& [Material, Pairs] : ByMaterial) {
            u64 Triangles = 0;
            for (const FOmmBakeJob* J : Pairs) Triangles += J->Mesh->Indices.size() / 3;
            Order.push_back({ Material, Triangles });
        }
        std::sort(Order.begin(), Order.end(),
                  [](const auto& A, const auto& B) { return A.second > B.second; });
        if (Order.size() > _MaxMaterials) Order.resize(_MaxMaterials);

        // O caminho do Renderer::RebakeStaleOpacityMicromaps: textura relida e alpha preparado UMA
        // vez por material (PrepareOmmAlphaFor), geometria relida e bake por par.
        std::printf("[OMM BENCH] rebake do editor por material (material: pares | triangulos | "
                    "geometria ms | textura+alpha ms | bake ms | total ms)\n");
        double SumGeo = 0, SumTex = 0, SumBake = 0;
        for (const auto& [Material, Triangles] : Order) {
            double GeoMs = 0, TexMs = 0, BakeMs = 0;
            const FOmmBakeJob& First = *ByMaterial[Material].front();
            FClock::time_point T = FClock::now();
            FTextureCPUData Alpha;
            const FTextureCPUData* AlphaPtr = nullptr;
            if (First.AlphaTexture) {
                if (!Store.LoadAlphaTexture(&TextureKeys[TextureIndex.at(First.AlphaTexture)], Alpha)) {
                    std::fprintf(stderr, "[OMM BENCH] LoadAlphaTexture falhou (material %u)\n",
                                 Material);
                    return 7;
                }
                AlphaPtr = &Alpha;
            }
            const auto Prepared =
                FOmmPreparedAlpha::Create(AlphaPtr, First.BaseAlpha, First.AlphaCutoff);
            TexMs += MsSince(T);

            for (const FOmmBakeJob* J : ByMaterial[Material]) {
                T = FClock::now();
                FOmmBakeInputStore::FLoadedGeometry Loaded;
                if (!Store.LoadGeometry(&MeshKeys[J->Key.MeshIndex], Loaded)) {
                    std::fprintf(stderr, "[OMM BENCH] LoadGeometry falhou (mesh %u)\n",
                                 J->Key.MeshIndex);
                    return 6;
                }
                GeoMs += MsSince(T);

                T = FClock::now();
                FOpacityMicromapBaker::Bake(Loaded.View(), *Prepared);
                BakeMs += MsSince(T);
            }
            std::printf("[OMM BENCH]   %4u: %3zu | %8llu | %8.1f | %8.1f | %8.1f | %8.1f\n",
                        Material, ByMaterial[Material].size(),
                        static_cast<unsigned long long>(Triangles), GeoMs, TexMs, BakeMs,
                        GeoMs + TexMs + BakeMs);
            SumGeo += GeoMs; SumTex += TexMs; SumBake += BakeMs;
        }
        std::printf("[OMM BENCH] soma dos %zu materiais: geometria %.0f ms, textura %.0f ms, "
                    "bake %.0f ms\n", Order.size(), SumGeo, SumTex, SumBake);
        return 0;
    }
}

int main(int argc, char** argv) {
    using namespace Smile;
    const char* ScenePath = nullptr;
    bool Force = false;
    bool Bench = false;
    for (int I = 1; I < argc; ++I) {
        if (std::strcmp(argv[I], "--force") == 0) Force = true;
        else if (std::strcmp(argv[I], "--bench-rebake") == 0) Bench = true;
        else if (!ScenePath) ScenePath = argv[I];
        else { ScenePath = nullptr; break; }
    }
    if (!ScenePath) {
        std::fprintf(stderr, "uso: SmileOmmBaker <cena.sscene> [--force | --bench-rebake]\n");
        return 2;
    }

    try {
        const auto Start = std::chrono::steady_clock::now();
        const auto Scene = LoadCookedSceneData(std::filesystem::path(ScenePath).wstring());
        if (!Scene) {
            std::fprintf(stderr, "falha ao carregar a cena: %s\n", ScenePath);
            return 3;
        }

        const std::vector<FOmmBakeJob> Jobs = CollectOmmBakeJobs(*Scene);
        // So mede: nao grava sidecar nenhum.
        if (Bench) return BenchRebake(*Scene, Jobs, 12);
        const std::filesystem::path OutPath = FOmmBakeCache::PathFor(Scene->BasePath);

        FOmmBakeCache Previous;
        if (!Force && std::filesystem::exists(OutPath)) {
            std::string Why;
            if (!Previous.Load(OutPath, Why))
                std::printf("[OMM BAKER] sidecar existente ignorado: %s\n", Why.c_str());
        }

        std::vector<std::shared_ptr<FOpacityMicromapData>> Results(Jobs.size());
        std::vector<size_t> Pending;
        u32 Reused = 0, Stale = 0;
        for (size_t I = 0; I < Jobs.size(); ++I) {
            switch (Previous.Lookup(Jobs[I].Key, Results[I])) {
                case FOmmBakeCache::ELookup::Hit:     ++Reused; break;
                case FOmmBakeCache::ELookup::Stale:   ++Stale; Pending.push_back(I); break;
                case FOmmBakeCache::ELookup::Missing: Pending.push_back(I); break;
            }
        }
        std::printf("[OMM BAKER] %zu pares mascarados: %u reaproveitados, %zu a bakear "
                    "(%u desatualizados)\n", Jobs.size(), Reused, Pending.size(), Stale);
        std::fflush(stdout);

        const FOmmBakeRunStats Run = BakeOmmJobs(Jobs, Pending, kOmmNoWorkloadLimit, Results);

        FOmmBakeCache Out;
        u64 Triangles = 0, Known = 0, Micromap = 0, Unknown = 0, ArrayBytes = 0, Excluded = 0;
        for (size_t I = 0; I < Jobs.size(); ++I) {
            const FOpacityMicromapData& D = *Results[I];
            Triangles  += D.TriangleCount;
            Known      += D.Coverage.FullyKnownTriangles;
            Micromap   += D.Coverage.MicromapTriangles;
            Unknown    += D.Coverage.FullyUnknownTriangles;
            ArrayBytes += D.ArrayData.size();
            Excluded   += D.WorkloadExcludedTriangles;
            Out.Put(Jobs[I].Key, Results[I]);
        }
        // Sem teto nao ha divisao: um excluido aqui significa que o baker de runtime vazou para
        // o caminho offline, e o sidecar gravaria exatamente o buraco que ele existe para fechar.
        if (Excluded != 0) {
            std::fprintf(stderr, "[OMM BAKER] %llu triangulos excluidos por workload num bake sem "
                                 "teto — nao gravando\n",
                         static_cast<unsigned long long>(Excluded));
            return 4;
        }

        std::string Why;
        if (!Out.Save(OutPath, Why)) {
            std::fprintf(stderr, "[OMM BAKER] falha ao gravar %s: %s\n", OutPath.string().c_str(),
                         Why.c_str());
            return 5;
        }

        const double TotalS =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
        const double N = Triangles ? static_cast<double>(Triangles) : 1.0;
        std::printf("[OMM BAKER] bake de %zu pares em %.1f s (%u grandes internos + %u pequenos em "
                    "%u workers); total %.1f s\n",
                    Pending.size(), Run.Ms / 1000.0, Run.InternalJobs, Run.ParallelJobs,
                    Run.Workers, TotalS);
        std::printf("[OMM BAKER] cobertura de %llu triangulos: %.1f%% resolvidos inteiros, %.1f%% com "
                    "micro-map, %.1f%% UNKNOWN inteiros; %.1f MB de array\n",
                    static_cast<unsigned long long>(Triangles), 100.0 * Known / N,
                    100.0 * Micromap / N, 100.0 * Unknown / N,
                    static_cast<double>(ArrayBytes) / (1024.0 * 1024.0));
        // Histograma por NIVEL de subdivisao: micro-maps, bytes de array e triangulos que os
        // referenciam. E o que diz se o teto de nivel (maxSubdivisionLevel) morde em alguma coisa —
        // o nivel de cada micro-map e escolhido pelo dynamicSubdivisionScale, e o teto so corta
        // quem pediria mais que ele.
        struct FLevel { u64 Maps = 0, Bytes = 0, Triangles = 0; };
        FLevel Levels[13]{};
        for (const auto& D : Results) {
            for (const auto& Desc : D->Descs) {
                const u32 L = std::min<u32>(Desc.SubdivisionLevel, 12u);
                const u64 Bits = Desc.Format == D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE ? 2 : 1;
                ++Levels[L].Maps;
                Levels[L].Bytes += ((1ull << (2 * L)) * Bits + 7) / 8;
            }
            for (u32 T = 0; T < D->TriangleCount; ++T) {
                const i32 Index = D->IndexOf(T);
                if (Index >= 0 && static_cast<size_t>(Index) < D->Descs.size())
                    ++Levels[std::min<u32>(D->Descs[Index].SubdivisionLevel, 12u)].Triangles;
            }
        }
        std::printf("[OMM BAKER] por nivel de subdivisao (nivel: micro-maps | KB de array | "
                    "triangulos com micro-map):\n");
        for (u32 L = 0; L <= 12; ++L)
            if (Levels[L].Maps)
                std::printf("[OMM BAKER]   %2u: %6llu | %9.1f | %8llu\n", L,
                            static_cast<unsigned long long>(Levels[L].Maps),
                            static_cast<double>(Levels[L].Bytes) / 1024.0,
                            static_cast<unsigned long long>(Levels[L].Triangles));
        std::printf("[OMM BAKER] gravado: %s\n", OutPath.string().c_str());
        return 0;
    } catch (const std::exception& E) {
        std::fprintf(stderr, "[OMM BAKER] falhou: %s\n", E.what());
        return 1;
    }
}
