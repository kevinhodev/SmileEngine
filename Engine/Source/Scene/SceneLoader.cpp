#include "Smile/Scene/SceneLoader.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Smile {
    namespace {
        using Clock = std::chrono::steady_clock;

        double MsSince(Clock::time_point Start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
        }

        bool ReadFile(const fs::path& Path, std::vector<u8>& Out) {
            std::ifstream f(Path, std::ios::binary);
            if (!f) return false;
            f.seekg(0, std::ios::end);
            std::streamoff sz = f.tellg();
            f.seekg(0, std::ios::beg);
            if (sz <= 0) return false;
            Out.resize(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(Out.data()), sz);
            return static_cast<bool>(f);
        }

        bool ArrayFits(size_t Offset, u64 Count, size_t Stride, size_t Total) {
            if (Offset > Total || Count > std::numeric_limits<size_t>::max() / Stride) return false;
            const size_t Bytes = static_cast<size_t>(Count) * Stride;
            return Bytes <= Total - Offset;
        }
    }

    FSceneImportResultPtr LoadCookedSceneData(const std::wstring& _ScenePath) {
        const Clock::time_point t0 = Clock::now();
        const fs::path Input(_ScenePath);
        const fs::path Base = Input.parent_path() / Input.stem();
        fs::path ScenePath = Base; ScenePath += L".sscene";
        fs::path MeshPath  = Base; MeshPath  += L".smesh";

        try {
            std::vector<u8> SceneBytes;
            std::vector<u8> MeshBytes;
            if (!ReadFile(ScenePath, SceneBytes)) {
                LogError("LoadCookedScene: nao abriu " + ScenePath.string());
                return {};
            }
            if (!ReadFile(MeshPath, MeshBytes)) {
                LogError("LoadCookedScene: nao abriu " + MeshPath.string());
                return {};
            }
            if (SceneBytes.size() < sizeof(SSceneHeader) || MeshBytes.size() < sizeof(SMeshHeader)) {
                LogError("LoadCookedScene: arquivos truncados");
                return {};
            }

            auto Imported = std::make_shared<FSceneImportResult>();
            Imported->BasePath  = Base;
            Imported->ScenePath = ScenePath;
            Imported->SceneDir  = Base.parent_path();
            std::memcpy(&Imported->SceneHeader, SceneBytes.data(), sizeof(SSceneHeader));
            std::memcpy(&Imported->MeshHeader, MeshBytes.data(), sizeof(SMeshHeader));
            const SSceneHeader& SceneHeader = Imported->SceneHeader;
            const SMeshHeader& MeshHeader = Imported->MeshHeader;
            if (SceneHeader.Magic != kSSceneMagic || MeshHeader.Magic != kSMeshMagic) {
                LogError("LoadCookedScene: magic invalido (o arquivo nao e um cozido da Smile)");
                return {};
            }
            // O runtime nao sintetiza dados ausentes de versoes antigas; a cena deve ser recozida.
            if (SceneHeader.Version != kCookedVersion || MeshHeader.Version != kCookedVersion) {
                LogError("LoadCookedScene: cozido v" +
                         std::to_string(MeshHeader.Version != kCookedVersion ? MeshHeader.Version
                                                                             : SceneHeader.Version) +
                         ", a engine exige v" + std::to_string(kCookedVersion) +
                         ". Recozinhe a cena com o SmileCooker (a v8 adiciona o payload de RT por"
                         " triangulo, que o runtime nao sintetiza).");
                return {};
            }

            const size_t MaterialsOffset = sizeof(SSceneHeader);
            if (!ArrayFits(MaterialsOffset, SceneHeader.MaterialCount,
                           sizeof(SSceneMaterial), SceneBytes.size())) {
                LogError("LoadCookedScene: tabela de materiais truncada");
                return {};
            }
            const size_t RenderablesOffset = MaterialsOffset +
                sizeof(SSceneMaterial) * SceneHeader.MaterialCount;
            if (!ArrayFits(RenderablesOffset, SceneHeader.RenderableCount,
                           sizeof(SSceneRenderable), SceneBytes.size())) {
                LogError("LoadCookedScene: tabela de renderaveis truncada");
                return {};
            }
            const size_t EntriesOffset = sizeof(SMeshHeader);
            if (!ArrayFits(EntriesOffset, MeshHeader.MeshCount,
                           sizeof(SMeshEntry), MeshBytes.size())) {
                LogError("LoadCookedScene: tabela de meshes truncada");
                return {};
            }
            const size_t GeometryOffset = EntriesOffset + sizeof(SMeshEntry) * MeshHeader.MeshCount;
            const size_t GeometryBytes = MeshBytes.size() - GeometryOffset;

            Imported->Materials.resize(SceneHeader.MaterialCount);
            if (SceneHeader.MaterialCount > 0) {
                std::memcpy(Imported->Materials.data(), SceneBytes.data() + MaterialsOffset,
                            sizeof(SSceneMaterial) * SceneHeader.MaterialCount);
            }
            Imported->Renderables.resize(SceneHeader.RenderableCount);
            if (SceneHeader.RenderableCount > 0) {
                std::memcpy(Imported->Renderables.data(), SceneBytes.data() + RenderablesOffset,
                            sizeof(SSceneRenderable) * SceneHeader.RenderableCount);
            }
            Imported->MeshEntries.resize(MeshHeader.MeshCount);
            if (MeshHeader.MeshCount > 0) {
                std::memcpy(Imported->MeshEntries.data(), MeshBytes.data() + EntriesOffset,
                            sizeof(SMeshEntry) * MeshHeader.MeshCount);
            }

            for (const SMeshEntry& Entry : Imported->MeshEntries) {
                if (Entry.VertexOffset > std::numeric_limits<size_t>::max() ||
                    Entry.IndexOffset > std::numeric_limits<size_t>::max() ||
                    Entry.RTTriangleOffset > std::numeric_limits<size_t>::max() ||
                    !ArrayFits(static_cast<size_t>(Entry.VertexOffset), Entry.VertexCount,
                               sizeof(Vertex), GeometryBytes) ||
                    !ArrayFits(static_cast<size_t>(Entry.IndexOffset), Entry.IndexCount,
                               sizeof(u32), GeometryBytes) ||
                    !ArrayFits(static_cast<size_t>(Entry.RTTriangleOffset), Entry.RTTriangleCount,
                               sizeof(FRTTriangle), GeometryBytes)) {
                    LogError("LoadCookedScene: blob de geometria truncado");
                    return {};
                }
                // RTTriangle[i] corresponde ao PrimitiveIndex i do BLAS.
                if (Entry.RTTriangleCount != Entry.IndexCount / 3u) {
                    LogError("LoadCookedScene: payload de RT com " +
                             std::to_string(Entry.RTTriangleCount) + " triangulos para " +
                             std::to_string(Entry.IndexCount / 3u) +
                             " do IB — cozido inconsistente, recozinhe a cena");
                    return {};
                }
            }
            Imported->ReadMs = MsSince(t0);

            struct FTextureFlags { bool SRGB; bool IsNormal; };
            std::unordered_map<std::string, FTextureFlags> UniquePaths;
            auto Consider = [&](const char* Relative, bool SRGB, bool IsNormal) {
                if (!Relative || !Relative[0]) return;
                UniquePaths.emplace(
                    std::string(Relative, strnlen(Relative, kCookedMaxPath)),
                    FTextureFlags{ SRGB, IsNormal });
            };
            for (const SSceneMaterial& Material : Imported->Materials) {
                Consider(Material.BaseColor, true,  false);
                Consider(Material.Emissive,  true,  false);
                Consider(Material.Specular,  false, false);
                Consider(Material.Normal,    false, true);
                Consider(Material.Metalness, false, false);
                Consider(Material.Roughness, false, false);
            }

            std::vector<FTextureFlags> TextureFlags;
            Imported->TexturePaths.reserve(UniquePaths.size());
            TextureFlags.reserve(UniquePaths.size());
            for (const auto& [Path, Flags] : UniquePaths) {
                Imported->TexturePaths.push_back(Path);
                TextureFlags.push_back(Flags);
            }
            Imported->TextureData.resize(Imported->TexturePaths.size());

            const Clock::time_point DecodeStart = Clock::now();
            const unsigned HardwareThreads = std::max(1u, std::thread::hardware_concurrency());
            // Reserva um core para o loader e outro para o event loop/render do Editor.
            const unsigned WorkerBudget = HardwareThreads > 2 ? HardwareThreads - 2 : 1;
            const unsigned WorkerCount = std::min<unsigned>(
                std::min(WorkerBudget, 8u), static_cast<unsigned>(Imported->TexturePaths.size()));
            auto DecodeWorker = [&](unsigned WorkerIndex) {
                for (size_t I = WorkerIndex; I < Imported->TexturePaths.size(); I += WorkerCount) {
                    try {
                        const fs::path Relative(Imported->TexturePaths[I]);
                        const std::wstring FullPath = (Imported->SceneDir / Relative).wstring();
                        std::string Extension = Relative.extension().string();
                        for (char& C : Extension) if (C >= 'A' && C <= 'Z') C += 32;
                        Imported->TextureData[I] = (Extension == ".dds")
                            ? FTexture::LoadDDSCPU(FullPath, TextureFlags[I].SRGB)
                            : FTexture::LoadCPU(
                                FullPath, TextureFlags[I].IsNormal, TextureFlags[I].SRGB);
                    } catch (const std::exception& Error) {
                        LogError("LoadCookedScene: falha ao preparar textura " +
                                 Imported->TexturePaths[I] + ": " + Error.what());
                    }
                }
            };
            std::vector<std::jthread> Workers;
            for (unsigned I = 0; I < WorkerCount; ++I) Workers.emplace_back(DecodeWorker, I);

            const Clock::time_point MeshStart = Clock::now();
            Imported->Meshes.resize(MeshHeader.MeshCount);
            const u8* GeometryBase = MeshBytes.data() + GeometryOffset;
            for (u32 I = 0; I < MeshHeader.MeshCount; ++I) {
                const SMeshEntry& Entry = Imported->MeshEntries[I];
                FMesh& Mesh = Imported->Meshes[I];
                Mesh.Vertices.resize(Entry.VertexCount);
                if (Entry.VertexCount > 0) {
                    std::memcpy(Mesh.Vertices.data(), GeometryBase + Entry.VertexOffset,
                                Entry.VertexCount * sizeof(Vertex));
                }
                Mesh.Indices.resize(Entry.IndexCount);
                if (Entry.IndexCount > 0) {
                    std::memcpy(Mesh.Indices.data(), GeometryBase + Entry.IndexOffset,
                                Entry.IndexCount * sizeof(u32));
                }
                // A ordem cozida ja corresponde ao PrimitiveIndex do BLAS.
                Mesh.RTTriangles.resize(Entry.RTTriangleCount);
                if (Entry.RTTriangleCount > 0) {
                    std::memcpy(Mesh.RTTriangles.data(), GeometryBase + Entry.RTTriangleOffset,
                                Entry.RTTriangleCount * sizeof(FRTTriangle));
                }
            }
            Imported->MeshMs = MsSince(MeshStart);

            for (std::jthread& Worker : Workers) Worker.join();
            Imported->DecodeMs = MsSince(DecodeStart);
            Imported->PrepareMs = MsSince(t0);
            LogDebug("Prepare scene CPU (ms): leitura=" + std::to_string((int)Imported->ReadMs) +
                     " decode=" + std::to_string((int)Imported->DecodeMs) +
                     " meshes=" + std::to_string((int)Imported->MeshMs) +
                     " | total=" + std::to_string((int)Imported->PrepareMs));
            return Imported;
        } catch (const std::exception& Error) {
            LogError("LoadCookedScene: falha na preparacao CPU: " + std::string(Error.what()));
            return {};
        }
    }
}
