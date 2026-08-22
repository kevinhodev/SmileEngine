#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RendererCaptureState.h"
#include "Smile/Graphics/Renderer/RendererSceneState.h"
#include "Smile/Graphics/Backend/RenderBackend.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Scene/SceneLoader.h"
#include "Smile/Graphics/Debug/VramTracker.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace Smile {
    namespace {
        using Clock = std::chrono::steady_clock;

        double MsSince(Clock::time_point Start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
        }
    }

    void Renderer::RecreateObjectCB() {
        Backend->DirectQueue.Flush();
        if (ObjectCB && MappedObjectCB) { ObjectCB->Unmap(0, nullptr); MappedObjectCB = nullptr; }
        ObjectCB.Reset();

        static_assert(sizeof(ObjectConstants) % 256 == 0,
                      "o CB de objeto e indexado por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            Backend->Device.Native(), sizeof(ObjectConstants),
            FCommandQueue::kFramesInFlight * MaxObjects);
        ObjectCB       = Upload.Resource;
        MappedObjectCB = Upload.Mapped;

        // ObjectCB e HiZ compartilham a mesma capacidade de objetos.
        HiZ.SetupObjects(Backend->Device.Native(), Backend->SRVHeap, MaxObjects);
    }

    bool Renderer::LoadCookedScene(const std::wstring& _ScenePath, bool _Additive) {
        return CommitCookedScene(LoadCookedSceneData(_ScenePath), _Additive);
    }

    bool Renderer::CommitCookedScene(std::shared_ptr<FSceneImportResult> _Imported, bool _Additive) {
        if (!_Imported) return false;
        // A captura nao pode atravessar uma mudanca de geometria ou de historicos.
        CaptureState->Session.Cancel("uma cena foi carregada durante o aquecimento");
        const Clock::time_point t0 = Clock::now();
        // As estatisticas cobrem apenas a fase GPU deste commit.
        GpuResources::ResetCreationStats();

        // Loads e resizes usam capturas separadas porque possuem perfis de alocacao distintos.
#if SMILE_DIAGNOSTICS
        const char* CaptureOverride = std::getenv("SMILE_CAPTURE_DESCS_LOAD");
        GpuResources::FDescCaptureSession DescCapture(
            CaptureOverride ? CaptureOverride : "smile-load-descs.txt");
#endif
        const FSceneImportResult& Imported = *_Imported;
        const fs::path& base = Imported.BasePath;
        const fs::path& scenePath = Imported.ScenePath;
        const fs::path& sceneDir = Imported.SceneDir;
        const SSceneHeader& sh = Imported.SceneHeader;
        const SMeshHeader& mh = Imported.MeshHeader;
        const auto& mats = Imported.Materials;
        const auto& rnds = Imported.Renderables;
        const auto& entries = Imported.MeshEntries;

        // A carga aditiva preserva os recursos e objetos existentes.
        if (!_Additive) {
            // Frames em voo ainda podem referenciar recursos, descritores e CBVs da cena anterior.
            Backend->DirectQueue.Flush();

            SceneState->Scene.Clear();
            for (auto& m : ImportedMaterials) m->Release(Backend->SRVHeap);
            ImportedMaterials.clear();
            for (auto& t : ImportedTextures) t->Release(Backend->SRVHeap);
            ImportedTextures.clear();
        }

        // PhaseSum permite separar cada fase e fechar o total nao atribuido ao final.
        GpuResources::FCreationStats PhaseSum{};
        const Clock::time_point TextureUploadStart = Clock::now();
        const auto TexCreationBase = GpuResources::CreationStats();
        std::vector<FTexture> texs = FTexture::CreateBatchFromCPU(
            Backend->Device.Native(), Backend->UploadQueue, Backend->SRVHeap, Imported.TextureData);
        const double msTexUpload = MsSince(TextureUploadStart);
        GpuResources::AccumulatePhase(PhaseSum, "commit/texturas", TexCreationBase);
        const std::vector<std::string>& relList = Imported.TexturePaths;
        std::unordered_map<std::string, FTexture*> texByPath;
        u32 uploaded = 0;
        for (size_t i = 0; i < relList.size(); ++i) {
            if (!texs[i].IsValid()) { texByPath[relList[i]] = nullptr; continue; }
            auto up = std::make_unique<FTexture>(std::move(texs[i]));
            texByPath[relList[i]] = up.get();
            ImportedTextures.push_back(std::move(up));
            ++uploaded;
        }

        auto getTex = [&](const char* rel) -> FTexture* {
            if (!rel || !rel[0]) return nullptr;
            auto it = texByPath.find(rel);
            return (it != texByPath.end()) ? it->second : nullptr;
        };

        // SSceneMaterial nao possui padding e o Cooker inicializa todos os bytes; o hash e estavel.
        auto MaterialContentId = [](const SSceneMaterial& _Sm) -> u64 {
            const auto* Bytes = reinterpret_cast<const u8*>(&_Sm);
            u64 H = 1469598103934665603ull;               // offset basis
            for (size_t b = 0; b < sizeof(SSceneMaterial); ++b) {
                H ^= Bytes[b];
                H *= 1099511628211ull;                    // prime
            }
            return H;
        };

        std::vector<FMaterial*> matPtrs(sh.MaterialCount, nullptr);
        for (u32 i = 0; i < sh.MaterialCount; ++i) {
            const SSceneMaterial& sm = mats[i];
            auto mat = std::make_unique<FMaterial>();
            mat->Name = std::string(sm.Name, strnlen(sm.Name, kCookedMaxName));
            mat->Id   = MaterialContentId(sm);

            FTexture* baseT  = getTex(sm.BaseColor);
            FTexture* specT  = getTex(sm.Specular);
            FTexture* normT  = getTex(sm.Normal);
            FTexture* emisT  = getTex(sm.Emissive);
            FTexture* metalT = getTex(sm.Metalness);
            FTexture* roughT = getTex(sm.Roughness);

            mat->Albedo            = baseT  ? baseT  : &TexDefaultWhite;
            mat->Normal            = normT  ? normT  : &TexDefaultNormal;
            mat->MetallicRoughness = specT  ? specT  : &TexDefaultWhite;
            mat->AO                = &TexDefaultWhite;
            mat->Emissive          = emisT  ? emisT  : &TexDefaultBlack;
            mat->Height            = &TexDefaultWhite;
            mat->Metalness         = metalT ? metalT : &TexDefaultWhite;
            mat->Roughness         = roughT ? roughT : &TexDefaultWhite;

            mat->Constants.BaseColorFactor = Vec4{ sm.BaseColorFactor[0], sm.BaseColorFactor[1],
                                                   sm.BaseColorFactor[2], sm.BaseColorFactor[3] };
            mat->Constants.EmissiveFactor  = Vec4{ sm.EmissiveFactor[0], sm.EmissiveFactor[1],
                                                   sm.EmissiveFactor[2], 1.0f };
            mat->Constants.EmissiveStrength = sm.EmissiveStrength;

            mat->Finalize(Backend->Device.Native(), Backend->SRVHeap);

            mat->Constants.HasAlbedoMap            = baseT ? 1u : 0u;
            mat->Constants.HasNormalMap            = normT ? 1u : 0u;
            mat->Constants.HasMetallicRoughnessMap = specT ? 1u : 0u;
            mat->Constants.HasAOMap                = 0u;
            mat->Constants.HasEmissiveMap          = emisT ? 1u : 0u;
            mat->Constants.HasHeightMap            = 0u;
            mat->Constants.HasMetalnessMap         = metalT ? 1u : 0u;
            mat->Constants.HasRoughnessMap         = roughT ? 1u : 0u;

            // Mapas usam fator neutro; sem mapa prevalece o fator cozido.
            mat->Constants.SpecularPacking    = specT ? 1u : 0u;
            mat->Constants.MetallicFactor     = (specT || metalT) ? 1.0f : sm.MetallicFactor;
            mat->Constants.RoughnessFactor    = (specT || roughT) ? 1.0f : sm.RoughnessFactor;

            mat->Blend = (sm.Blend != 0u);

            mat->Constants.AOStrength         = 0.0f;
            mat->Constants.NormalFlipY        = 1u;
            mat->Constants.NormalReconstructZ =
                (normT && normT->Format() == DXGI_FORMAT_BC5_UNORM) ? 1u : 0u;
            mat->Constants.AlphaTest          = sm.AlphaTest;
            mat->Constants.AlphaCutoff        = sm.AlphaCutoff;
            mat->TwoSided                     = (sm.TwoSided != 0);

            // Masked/two-sided nao implica folhagem; o Cooker fornece o modelo explicitamente.
            const bool isFoliage = (sm.Foliage != 0u);
            mat->Constants.ShadingModel   = isFoliage ? 1u : 0u;
            mat->Constants.SubsurfaceColor = { 1.0f, 1.0f, 1.0f, isFoliage ? 0.6f : 0.0f };

            mat->UpdateConstants();
            matPtrs[i] = mat.get();
            ImportedMaterials.push_back(std::move(mat));
        }

        auto matOf = [&](u32 mi) -> FMaterial* {
            return (mi != kNoMaterial && mi < sh.MaterialCount) ? matPtrs[mi] : nullptr;
        };
        // Assets sem nome usam material e indice como fallbacks estaveis no Outliner.
        auto nameOf = [&](const SSceneRenderable& r, u32 mi, u32 fallbackIdx) -> std::string {
            if (r.Name[0] != '\0') return std::string(r.Name, strnlen(r.Name, kCookedMaxName));
            if (mi != kNoMaterial && mi < sh.MaterialCount && mats[mi].Name[0] != '\0') {
                const char* n = mats[mi].Name;
                return std::string(n, strnlen(n, kCookedMaxName));
            }
            return "Mesh " + std::to_string(fallbackIdx);
        };

        const Clock::time_point tMeshUploadStart = Clock::now();
        const auto MeshCreationBase = GpuResources::CreationStats();
        std::vector<FGpuMesh*> meshPtrs = SceneState->Scene.AddMeshesBatch(
            Backend->Device.Native(), Backend->UploadQueue, Imported.Meshes);
        for (u32 i = 0; i < sh.RenderableCount; ++i) {
            const SSceneRenderable& r = rnds[i];
            if (r.MeshIndex >= mh.MeshCount) continue;
            FRenderable out;
            out.Name     = nameOf(r, r.MaterialIndex, i);
            out.Mesh     = meshPtrs[r.MeshIndex];
            out.Material = matOf(r.MaterialIndex);
            out.Transform.Position      = Vec3{ r.Position[0], r.Position[1], r.Position[2] };
            out.Transform.RotationEuler = Vec3{ r.RotationEuler[0], r.RotationEuler[1],
                                                r.RotationEuler[2] };
            out.Transform.Scale         = Vec3{ r.Scale[0], r.Scale[1], r.Scale[2] };
            // O arquivo armazena AABB local; os consumidores usam bounds de mundo.
            const SMeshEntry& e = entries[r.MeshIndex];
            out.LocalAABBMin = Vec3{ e.AABBMin[0], e.AABBMin[1], e.AABBMin[2] };
            out.LocalAABBMax = Vec3{ e.AABBMax[0], e.AABBMax[1], e.AABBMax[2] };
            out.RefreshWorldBounds();
            // CookedIndex preserva a identidade do asset quando a lista viva muda.
            out.CookedIndex = static_cast<i32>(i);
            SceneState->Scene.AddRenderable(out);
        }

        const double msMeshUpload = MsSince(tMeshUploadStart);
        GpuResources::AccumulatePhase(PhaseSum, "commit/meshes", MeshCreationBase);

        // BLAS, DDGI e o frame so podem consumir os uploads apos esta sincronizacao unica.
        const Clock::time_point tSyncStart = Clock::now();
        Backend->UploadQueue.WaitIdle();
        const double msSync = MsSince(tSyncStart);

        const Clock::time_point ObjectSetupStart = Clock::now();
        if (static_cast<u32>(SceneState->Scene.Renderables().size()) > MaxObjects) {
            // A folga permite criar objetos sem redimensionar ObjectCB e HiZ imediatamente.
            MaxObjects = SceneCapacityFor(static_cast<u32>(SceneState->Scene.Renderables().size()));
            RecreateObjectCB();
        }

        // Uma substituicao reposiciona a camera e invalida historicos temporais.
        if (!_Additive) {
            SceneState->Camera.SetPose(Vec3{ -14.476486f, 3.932823f, 0.278743f }, -9.05f, 78.75f);
            Settings().NotifyCameraCut();
        }
        const double msObjectSetup = MsSince(ObjectSetupStart);

        LogDebug("Assets da cena preparados: " + std::to_string(mh.MeshCount) + " meshes, " +
                 std::to_string(sh.MaterialCount) + " materiais, " +
                 std::to_string(sh.RenderableCount) + " renderaveis, " +
                 std::to_string(uploaded) + " texturas DDS");
        // O volume de GI cobre tambem objetos preservados por uma carga aditiva.
        Vec3 sceneMin{  1e30f,  1e30f,  1e30f };
        Vec3 sceneMax{ -1e30f, -1e30f, -1e30f };
        for (const FRenderable& r : SceneState->Scene.Renderables()) {
            sceneMin.X = std::min(sceneMin.X, r.AABBMin.X);
            sceneMin.Y = std::min(sceneMin.Y, r.AABBMin.Y);
            sceneMin.Z = std::min(sceneMin.Z, r.AABBMin.Z);
            sceneMax.X = std::max(sceneMax.X, r.AABBMax.X);
            sceneMax.Y = std::max(sceneMax.Y, r.AABBMax.Y);
            sceneMax.Z = std::max(sceneMax.Z, r.AABBMax.Z);
        }

        // O sidecar <cena>.terrain.json usa chaves planas e caminhos relativos a cena.
        const Clock::time_point TerrainStart = Clock::now();
        const auto TerrainCreationBase = GpuResources::CreationStats();
        {
            fs::path terrainPath = base; terrainPath += L".terrain.json";
            if (fs::exists(terrainPath)) {
                std::ifstream tf(terrainPath);
                std::string js((std::istreambuf_iterator<char>(tf)),
                               std::istreambuf_iterator<char>());
                auto FindNum = [&](const char* key, f32 def) -> f32 {
                    const std::string k = std::string("\"") + key + "\"";
                    size_t p = js.find(k);
                    if (p == std::string::npos) return def;
                    p = js.find(':', p + k.size());
                    if (p == std::string::npos) return def;
                    return std::strtof(js.c_str() + p + 1, nullptr);
                };
                auto FindStr = [&](const char* key) -> std::string {
                    const std::string k = std::string("\"") + key + "\"";
                    size_t p = js.find(k);
                    if (p == std::string::npos) return {};
                    p = js.find(':', p + k.size());
                    if (p == std::string::npos) return {};
                    p = js.find('"', p);
                    if (p == std::string::npos) return {};
                    const size_t e = js.find('"', p + 1);
                    if (e == std::string::npos) return {};
                    return js.substr(p + 1, e - p - 1);
                };
                const std::string hm = FindStr("heightmap");
                if (!hm.empty()) {
                    FTerrainDesc td;
                    td.HeightmapPath = (sceneDir / fs::path(hm)).wstring();
                    td.HeightmapSize = static_cast<u32>(FindNum("size", 0.0f));
                    td.UnitsPerTexel = FindNum("unitsPerTexel", 1.0f);
                    td.HeightScale   = FindNum("heightScale", 100.0f);
                    td.Origin        = { FindNum("originX", 0.0f), FindNum("originY", 0.0f),
                                         FindNum("originZ", 0.0f) };
                    // As quatro camadas usam sufixos numericos para textura, normal, tile e roughness.
                    for (u32 l = 0; l < FTerrainDesc::kLayers; ++l) {
                        const std::string suf = std::to_string(l);
                        const std::string alb = FindStr(("tex" + suf).c_str());
                        const std::string nrm = FindStr(("nrm" + suf).c_str());
                        if (!alb.empty())
                            td.LayerAlbedo[l] = (sceneDir / fs::path(alb)).wstring();
                        if (!nrm.empty())
                            td.LayerNormal[l] = (sceneDir / fs::path(nrm)).wstring();
                        td.LayerTile[l]  = FindNum(("tile" + suf).c_str(), td.LayerTile[l]);
                        td.LayerRough[l] = FindNum(("rough" + suf).c_str(), td.LayerRough[l]);
                    }
                    td.RockSlopeStart = FindNum("rockSlopeStart", td.RockSlopeStart);
                    td.RockSlopeEnd   = FindNum("rockSlopeEnd",   td.RockSlopeEnd);
                    td.DirtScale      = FindNum("dirtScale",      td.DirtScale);
                    td.DirtAmount     = FindNum("dirtAmount",     td.DirtAmount);
                    td.HighStart      = FindNum("highStart",      td.HighStart);
                    td.HighEnd        = FindNum("highEnd",        td.HighEnd);
                    td.BlendContrast  = FindNum("blendContrast",  td.BlendContrast);
                    td.MacroAmount    = FindNum("macroAmount",    td.MacroAmount);
                    if (Terrain.Load(Backend->Device.Native(), Backend->UploadQueue, Backend->SRVHeap, td)) {
                        // O proxy participa apenas da TLAS; FTerrain continua responsavel pelo raster.
                        FMesh Proxy;
                        if (Terrain.BuildProxyMesh(Proxy)) {
                            std::vector<FMesh> ProxyList;
                            ProxyList.push_back(std::move(Proxy));
                            std::vector<FGpuMesh*> ProxyMesh =
                                SceneState->Scene.AddMeshesBatch(Backend->Device.Native(), Backend->UploadQueue, ProxyList);
                            Backend->UploadQueue.WaitIdle(); // O BLAS consome este VB logo abaixo.

                            // A textura e o material do proxy compartilham o lifetime da cena importada.
                            FTexture* proxyAlbedo = nullptr;
                            FTextureCPUData proxyAlbedoCPU;
                            if (Terrain.TakeProxyAlbedoCPU(proxyAlbedoCPU)) {
                                auto tex = std::make_unique<FTexture>(
                                    FTexture::CreateFromCPU(Backend->Device.Native(), Backend->UploadQueue, Backend->SRVHeap,
                                                            proxyAlbedoCPU, EVramCategory::Terrain));
                                if (tex->IsValid()) {
                                    proxyAlbedo = tex.get();
                                    ImportedTextures.push_back(std::move(tex));
                                }
                            }

                            auto mat = std::make_unique<FMaterial>();
                            mat->Albedo            = proxyAlbedo ? proxyAlbedo : &TexDefaultWhite;
                            mat->Normal            = &TexDefaultNormal;
                            mat->MetallicRoughness = &TexDefaultWhite;
                            mat->AO                = &TexDefaultWhite;
                            mat->Emissive          = &TexDefaultBlack;
                            mat->Height            = &TexDefaultWhite;
                            mat->Metalness         = &TexDefaultWhite;
                            mat->Roughness         = &TexDefaultWhite;
                            // A textura bakeada exige fator neutro no hit shading.
                            mat->Constants.BaseColorFactor = proxyAlbedo
                                ? Vec4{ 1.0f, 1.0f, 1.0f, 1.0f }
                                : Vec4{ 0.26f, 0.32f, 0.19f, 1.0f };
                            mat->Constants.MetallicFactor  = 0.0f;
                            mat->Constants.RoughnessFactor = 0.95f;
                            mat->Finalize(Backend->Device.Native(), Backend->SRVHeap);
                            mat->UpdateConstants();

                            FRenderable proxy;
                            proxy.Name           = "TerrainRTProxy";
                            proxy.Mesh           = ProxyMesh.empty() ? nullptr : ProxyMesh[0];
                            proxy.Material       = mat.get();
                            proxy.RaytracingOnly = true;
                            // Com transform identidade, os bounds locais e de mundo devem coincidir.
                            Terrain.GetBounds(proxy.LocalAABBMin, proxy.LocalAABBMax);
                            proxy.AABBMin = proxy.LocalAABBMin;
                            proxy.AABBMax = proxy.LocalAABBMax;
                            if (proxy.Mesh) {
                                SceneState->Scene.AddRenderable(proxy);
                                ImportedMaterials.push_back(std::move(mat));
                            }
                        }
                    }
                } else {
                    LogError("Terreno: sidecar sem chave \"heightmap\": " + terrainPath.string());
                }
            } else if (!_Additive) {
                Terrain.Unload(Backend->SRVHeap);
            }
        }
        const double msTerrain = MsSince(TerrainStart);
        GpuResources::AccumulatePhase(PhaseSum, "commit/terreno", TerrainCreationBase);

        // Incluir terrenos extensos diluiria o grid DDGI; fora dele vale o fallback de ambiente.
        const Clock::time_point RaytracingStart = Clock::now();
        const auto RtCreationBase = GpuResources::CreationStats();
        BuildRaytracingScene();
        const double msRaytracing = MsSince(RaytracingStart);
        GpuResources::AccumulatePhase(PhaseSum, "commit/blasTlas", RtCreationBase);

        const Clock::time_point GIStart = Clock::now();
        const auto GiCreationBase = GpuResources::CreationStats();
        SetupGIForScene(sceneMin, sceneMax);
        const double msGI = MsSince(GIStart);
        GpuResources::AccumulatePhase(PhaseSum, "commit/setupGI", GiCreationBase);

        // O Editor repovoa as luzes da cena substituida pelo sidecar .lights.json.
        if (!_Additive) ClearLightSelection();

        const double CommitMs = MsSince(t0);
        LogDebug("Commit scene GPU (ms): uploadTex=" + std::to_string((int)msTexUpload) +
                " uploadMeshes=" + std::to_string((int)msMeshUpload) +
                " syncUpload=" + std::to_string((int)msSync) +
                " objects=" + std::to_string((int)msObjectSetup) +
                " terrain=" + std::to_string((int)msTerrain) +
                " blasTlas=" + std::to_string((int)msRaytracing) +
                " setupGI=" + std::to_string((int)msGI) +
                " | total=" + std::to_string((int)CommitMs));
        const auto TotalMs = static_cast<long long>(Imported.PrepareMs + CommitMs);
        LogInfo(std::string(_Additive ? "Cena adicionada" : "Cena carregada") + " em " +
                std::to_string(TotalMs) + " ms: " + scenePath.filename().string() + " | " +
                std::to_string(mh.MeshCount) + " meshes, " +
                std::to_string(sh.MaterialCount) + " materiais, " +
                std::to_string(sh.RenderableCount) + " renderaveis, " +
                std::to_string(uploaded) + " texturas");
        // Neste ponto o breakdown inclui texturas, meshes, ray tracing e GI.
        VramTracker::LogBreakdown(Backend->Device.QueryVideoMemory().LocalUsage);
        GpuResources::LogCreationUnattributed("commit/nao-atribuido", PhaseSum);
        GpuResources::LogCreationStats("load da cena");
#if SMILE_DIAGNOSTICS
        DescCapture.Complete();
#endif
        return true;
    }
}
