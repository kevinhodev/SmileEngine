#include "Smile/Graphics/Renderer.h"
#include "Smile/Scene/CookedFormat.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <thread>
#include <cmath>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

namespace Smile {

    void Renderer::RecreateObjectCB() {
        CommandQueue.Flush(); 
        if (ObjectCB && MappedObjectCB) { ObjectCB->Unmap(0, nullptr); MappedObjectCB = nullptr; }
        ObjectCB.Reset();

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC d{};
        d.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                             MaxObjects * sizeof(ObjectConstants);
        d.Height           = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels        = 1;
        d.Format           = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc       = { 1, 0 };
        d.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags            = D3D12_RESOURCE_FLAG_NONE;

        SMILE_HR(Device.Native()->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ObjectCB)));
        D3D12_RANGE NoRead{ 0, 0 };
        void* p = nullptr;
        SMILE_HR(ObjectCB->Map(0, &NoRead, &p));
        MappedObjectCB = reinterpret_cast<u8*>(p);

        // Buffers de bounds/visibilidade do occlusion culling acompanham a capacidade
        // (a fila ja foi flushada acima; recriar aqui e seguro).
        HiZ.SetupObjects(Device.Native(), SRVHeap, MaxObjects);
    }

    namespace {
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
    }

    bool Renderer::LoadCookedScene(const std::wstring& _ScenePath, bool _Additive) {
        fs::path in(_ScenePath);
        fs::path base = in.parent_path() / in.stem(); 
        fs::path scenePath = base; scenePath += L".sscene";
        fs::path meshPath  = base; meshPath  += L".smesh";
        fs::path sceneDir  = base.parent_path();

        using Clock = std::chrono::steady_clock;
        auto MsSince = [](Clock::time_point a) {
            return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        };
        const Clock::time_point t0 = Clock::now();

        std::vector<u8> sceneBytes, meshBytes;
        if (!ReadFile(scenePath, sceneBytes)) {
            LogError("LoadCookedScene: nao abriu " + scenePath.string());
            return false;
        }
        if (!ReadFile(meshPath, meshBytes)) {
            LogError("LoadCookedScene: nao abriu " + meshPath.string());
            return false;
        }
        if (sceneBytes.size() < sizeof(SSceneHeader) || meshBytes.size() < sizeof(SMeshHeader)) {
            LogError("LoadCookedScene: arquivos truncados");
            return false;
        }

        SSceneHeader sh{}; std::memcpy(&sh, sceneBytes.data(), sizeof(sh));
        SMeshHeader  mh{}; std::memcpy(&mh, meshBytes.data(),  sizeof(mh));
        if (sh.Magic != kSSceneMagic || sh.Version != kCookedVersion ||
            mh.Magic != kSMeshMagic  || mh.Version != kCookedVersion) {
            LogError("LoadCookedScene: magic/versao invalidos");
            return false;
        }

        const double msRead = MsSince(t0);

        const auto* mats = reinterpret_cast<const SSceneMaterial*>(sceneBytes.data() + sizeof(SSceneHeader));
        const auto* rnds = reinterpret_cast<const SSceneRenderable*>(
            sceneBytes.data() + sizeof(SSceneHeader) + sizeof(SSceneMaterial) * sh.MaterialCount);
        const auto* entriesRaw = meshBytes.data() + sizeof(SMeshHeader);
        const u8*   geoBase    = meshBytes.data() + sizeof(SMeshHeader) + sizeof(SMeshEntry) * mh.MeshCount;

        // Em carga aditiva preservamos meshes/materiais/texturas ja carregados; so
        // limpamos a cena anterior no modo de substituicao (padrao).
        if (!_Additive) {
            Scene.Clear();
            for (auto& m : ImportedMaterials) m->Release(SRVHeap);
            ImportedMaterials.clear();
            for (auto& t : ImportedTextures) t->Release(SRVHeap);
            ImportedTextures.clear();
        }

        struct TexLoad { bool srgb; bool isNormal; };
        std::unordered_map<std::string, TexLoad> uniquePaths;
        auto consider = [&](const char* rel, bool srgb, bool isNormal) {
            if (rel && rel[0]) uniquePaths.emplace(std::string(rel), TexLoad{ srgb, isNormal });
        };
        for (u32 i = 0; i < sh.MaterialCount; ++i) {
            consider(mats[i].BaseColor, true,  false);
            consider(mats[i].Emissive,  true,  false);
            consider(mats[i].Specular,  false, false);
            consider(mats[i].Normal,    false, true);
            consider(mats[i].Metalness, false, false);
            consider(mats[i].Roughness, false, false);
        }

        std::vector<std::string> relList;
        std::vector<TexLoad>     flagList;
        relList.reserve(uniquePaths.size());
        for (auto& kv : uniquePaths) { relList.push_back(kv.first); flagList.push_back(kv.second); }

        std::vector<FTextureCPUData> cpuData(relList.size());
        {
            unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            unsigned nthreads = std::min<unsigned>(hw, static_cast<unsigned>(relList.size()));
            auto worker = [&](unsigned tid) {
                for (size_t i = tid; i < relList.size(); i += nthreads) {
                    std::wstring full = (sceneDir / fs::path(relList[i])).wstring();
                    // DDS = formato BC pre-cozido; qualquer outra extensao (PNG/TGA/...) vai pelo WIC.
                    std::string ext = fs::path(relList[i]).extension().string();
                    for (char& c : ext) if (c >= 'A' && c <= 'Z') c += 32;
                    cpuData[i] = (ext == ".dds")
                        ? FTexture::LoadDDSCPU(full, flagList[i].srgb)
                        : FTexture::LoadCPU(full, flagList[i].isNormal, flagList[i].srgb);
                }
            };
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker, t);
            for (auto& th : pool) th.join();
        }
        const Clock::time_point tDecodeEnd = Clock::now();
        const double msDecode = MsSince(t0) - msRead;

        std::vector<FTexture> texs =
            FTexture::CreateBatchFromCPU(Device.Native(), UploadQueue, SRVHeap, cpuData);
        const double msTexUpload = MsSince(tDecodeEnd);
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

        std::vector<FMaterial*> matPtrs(sh.MaterialCount, nullptr);
        for (u32 i = 0; i < sh.MaterialCount; ++i) {
            const SSceneMaterial& sm = mats[i];
            auto mat = std::make_unique<FMaterial>();
            mat->Name = std::string(sm.Name, strnlen(sm.Name, kCookedMaxName));

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

            mat->Finalize(Device.Native(), SRVHeap); 

            mat->Constants.HasAlbedoMap            = baseT ? 1u : 0u;
            mat->Constants.HasNormalMap            = normT ? 1u : 0u;
            mat->Constants.HasMetallicRoughnessMap = specT ? 1u : 0u;
            mat->Constants.HasAOMap                = 0u;
            mat->Constants.HasEmissiveMap          = emisT ? 1u : 0u;
            mat->Constants.HasHeightMap            = 0u;
            mat->Constants.HasMetalnessMap         = metalT ? 1u : 0u;
            mat->Constants.HasRoughnessMap         = roughT ? 1u : 0u;

            // O fator multiplica o mapa no shader (Metallic = Factor * map.r); precisa ser 1 quando
            // existe mapa (packed Specular OU Metalness/Roughness separados) p/ nao zerar o produto.
            // Sem mapa, usa o fator cozido do material (ex.: vidro rough=0 reflexivo, lampada ~0.4).
            mat->Constants.SpecularPacking    = specT ? 1u : 0u;
            mat->Constants.MetallicFactor     = (specT || metalT) ? 1.0f : sm.MetallicFactor;
            mat->Constants.RoughnessFactor    = (specT || roughT) ? 1.0f : sm.RoughnessFactor;

            // Translucido (alpha-blend no passe forward); o alpha vem de BaseColorFactor.w (× textura).
            mat->Blend = (sm.Blend != 0u);

            mat->Constants.AOStrength         = 0.0f;
            mat->Constants.NormalFlipY        = 1u;              
            mat->Constants.NormalReconstructZ =
                (normT && normT->Format() == DXGI_FORMAT_BC5_UNORM) ? 1u : 0u;
            mat->Constants.AlphaTest          = sm.AlphaTest;
            mat->Constants.AlphaCutoff        = sm.AlphaCutoff;
            mat->TwoSided                     = (sm.TwoSided != 0);

            // Folhagem agora vem de um flag PROPRIO (decidido por keyword de vegetacao no cooker),
            // desacoplado de masked/two-sided — um cutout (corrente/grade) e masked mas DefaultLit.
            const bool isFoliage = (sm.Foliage != 0u);
            mat->Constants.ShadingModel   = isFoliage ? 1u : 0u;
            mat->Constants.SubsurfaceColor = { 1.0f, 1.0f, 1.0f, isFoliage ? 0.6f : 0.0f };

            mat->UpdateConstants();
            matPtrs[i] = mat.get();
            ImportedMaterials.push_back(std::move(mat));
        }

        std::vector<SMeshEntry> entries(mh.MeshCount);
        for (u32 i = 0; i < mh.MeshCount; ++i)
            std::memcpy(&entries[i], entriesRaw + sizeof(SMeshEntry) * i, sizeof(SMeshEntry));
        auto matOf = [&](u32 mi) -> FMaterial* {
            return (mi != kNoMaterial && mi < sh.MaterialCount) ? matPtrs[mi] : nullptr;
        };
        // Nome do renderable p/ o Scene Outliner: o cozido v6 nao guarda nome por no (o cooker
        // baka o transform no vertice), entao o melhor nome disponivel e o do material.
        auto nameOf = [&](u32 mi, u32 fallbackIdx) -> std::string {
            if (mi != kNoMaterial && mi < sh.MaterialCount && mats[mi].Name[0] != '\0') {
                const char* n = mats[mi].Name;
                return std::string(n, strnlen(n, kCookedMaxName));
            }
            return "Mesh " + std::to_string(fallbackIdx);
        };

        const Clock::time_point tMeshStart = Clock::now();
        std::vector<FMesh> meshesCPU(mh.MeshCount);
        for (u32 i = 0; i < mh.MeshCount; ++i) {
            const SMeshEntry& e = entries[i];
            meshesCPU[i].Vertices.resize(e.VertexCount);
            std::memcpy(meshesCPU[i].Vertices.data(), geoBase + e.VertexOffset, e.VertexCount * sizeof(Vertex));
            meshesCPU[i].Indices.resize(e.IndexCount);
            std::memcpy(meshesCPU[i].Indices.data(), geoBase + e.IndexOffset, e.IndexCount * sizeof(u32));
        }
        std::vector<FGpuMesh*> meshPtrs = Scene.AddMeshesBatch(Device.Native(), UploadQueue, meshesCPU);
        for (u32 i = 0; i < sh.RenderableCount; ++i) {
            const SSceneRenderable& r = rnds[i];
            if (r.MeshIndex >= mh.MeshCount) continue;
            FRenderable out;
            out.Name     = nameOf(r.MaterialIndex, i);
            out.Mesh     = meshPtrs[r.MeshIndex];
            out.Material = matOf(r.MaterialIndex);
            const SMeshEntry& e = entries[r.MeshIndex];
            out.AABBMin = Vec3{ e.AABBMin[0], e.AABBMin[1], e.AABBMin[2] };
            out.AABBMax = Vec3{ e.AABBMax[0], e.AABBMax[1], e.AABBMax[2] };
            Scene.AddRenderable(out);
        }

        const double msMesh = MsSince(tMeshStart);

        // Todos os uploads (texturas + meshes) foram submetidos SEM bloquear na fila COPY;
        // espera aqui, uma unica vez, antes do primeiro consumo (BLAS/DDGI/frame leem VB e SRV).
        const Clock::time_point tSyncStart = Clock::now();
        UploadQueue.WaitIdle();
        const double msSync = MsSince(tSyncStart);

        // Dimensiona o ObjectCB pelo total de renderaveis da cena (cobre carga aditiva,
        // onde os renderaveis do interior se somam aos do exterior ja presentes).
        if (static_cast<u32>(Scene.Renderables().size()) > MaxObjects) {
            MaxObjects = static_cast<u32>(Scene.Renderables().size());
            RecreateObjectCB();
        }

        // Em carga aditiva mantemos a camera onde o usuario deixou; so reposicionamos
        // na entrada de uma cena nova (substituicao).
        if (!_Additive)
            Camera.SetPose(Vec3{ -14.476486f, 3.932823f, 0.278743f }, -9.05f, 78.75f);

        LogInfo("Cena carregada: " + std::to_string(mh.MeshCount) + " meshes, " +
                std::to_string(sh.MaterialCount) + " materiais, " +
                std::to_string(sh.RenderableCount) + " renderaveis, " +
                std::to_string(uploaded) + " texturas DDS.");
        LogInfo("Load (ms): leitura=" + std::to_string((int)msRead) +
                " decode=" + std::to_string((int)msDecode) +
                " uploadTex=" + std::to_string((int)msTexUpload) +
                " meshes=" + std::to_string((int)msMesh) +
                " syncUpload=" + std::to_string((int)msSync) +
                " | total=" + std::to_string((int)MsSince(t0)));

        // AABB de uniao sobre TODA a cena (exterior + interior em carga aditiva), para
        // o volume de GI cobrir tudo que esta carregado.
        Vec3 sceneMin{  1e30f,  1e30f,  1e30f };
        Vec3 sceneMax{ -1e30f, -1e30f, -1e30f };
        for (const FRenderable& r : Scene.Renderables()) {
            sceneMin.X = std::min(sceneMin.X, r.AABBMin.X);
            sceneMin.Y = std::min(sceneMin.Y, r.AABBMin.Y);
            sceneMin.Z = std::min(sceneMin.Z, r.AABBMin.Z);
            sceneMax.X = std::max(sceneMax.X, r.AABBMax.X);
            sceneMax.Y = std::max(sceneMax.Y, r.AABBMax.Y);
            sceneMax.Z = std::max(sceneMax.Z, r.AABBMax.Z);
        }

        // Terreno (F1): sidecar <cena>.terrain.json ao lado da cena — JSON PLANO, so
        // chaves de primeiro nivel ("heightmap" relativo a pasta da cena, "size",
        // "unitsPerTexel", "heightScale", "originX/Y/Z"). Sem sidecar em carga de
        // substituicao, descarrega o terreno anterior.
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
                    // F2: camadas de material ("tex0".."tex3" albedo, "nrm0".."nrm3" normal,
                    // "tile0".."tile3" metros por tile, "rough0".."rough3") + regras dos
                    // pesos procedurais. Paths relativos a pasta da cena.
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
                    if (Terrain.Load(Device.Native(), UploadQueue, SRVHeap, td)) {
                        // F3: proxy do terreno na TLAS — renderable RaytracingOnly (fora do
                        // raster/CSM/picking; o terreno real rasteriza pelo FTerrain). Entra
                        // ANTES do BuildRaytracingScene logo abaixo, e DEPOIS da uniao de
                        // AABB da cena (o volume de GI nao estica pro terreno de proposito).
                        // Material = cor media do vale (e o que o bounce do GI enxerga).
                        FMesh Proxy;
                        if (Terrain.BuildProxyMesh(Proxy)) {
                            std::vector<FMesh> ProxyList;
                            ProxyList.push_back(std::move(Proxy));
                            std::vector<FGpuMesh*> ProxyMesh =
                                Scene.AddMeshesBatch(Device.Native(), UploadQueue, ProxyList);
                            UploadQueue.WaitIdle(); // BLAS le o VB logo abaixo

                            auto mat = std::make_unique<FMaterial>();
                            mat->Albedo            = &TexDefaultWhite;
                            mat->Normal            = &TexDefaultNormal;
                            mat->MetallicRoughness = &TexDefaultWhite;
                            mat->AO                = &TexDefaultWhite;
                            mat->Emissive          = &TexDefaultBlack;
                            mat->Height            = &TexDefaultWhite;
                            mat->Metalness         = &TexDefaultWhite;
                            mat->Roughness         = &TexDefaultWhite;
                            mat->Constants.BaseColorFactor = { 0.26f, 0.32f, 0.19f, 1.0f };
                            mat->Constants.MetallicFactor  = 0.0f;
                            mat->Constants.RoughnessFactor = 0.95f;
                            mat->Finalize(Device.Native(), SRVHeap);
                            mat->UpdateConstants();

                            FRenderable proxy;
                            proxy.Name           = "TerrainRTProxy";
                            proxy.Mesh           = ProxyMesh.empty() ? nullptr : ProxyMesh[0];
                            proxy.Material       = mat.get();
                            proxy.RaytracingOnly = true;
                            Terrain.GetBounds(proxy.AABBMin, proxy.AABBMax);
                            if (proxy.Mesh) {
                                Scene.AddRenderable(proxy);
                                ImportedMaterials.push_back(std::move(mat));
                            }
                        }
                    }
                } else {
                    LogError("Terreno: sidecar sem chave \"heightmap\": " + terrainPath.string());
                }
            } else if (!_Additive) {
                Terrain.Unload(SRVHeap);
            }
        }

        // O volume de GI NAO inclui o terreno de proposito: um terreno de km esticaria o
        // grid de probes do DDGI. Fora do volume o shading cai no fallback de ambiente;
        // terreno no GI de verdade vem na F3 (BLAS proxy na TLAS).
        BuildRaytracingScene();
        SetupGIForScene(sceneMin, sceneMax);

        // Luzes puntuais: a carga nao-aditiva limpou a cena (Scene.Clear); o EDITOR repovoa
        // pelo <cena>.lights.json (LightsBridge::OnSceneLoaded) e invalida a selecao de luz.
        if (!_Additive) ClearLightSelection();
        return true;
    }
}
