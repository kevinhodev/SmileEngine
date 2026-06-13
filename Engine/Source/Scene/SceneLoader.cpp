#include "Smile/Graphics/Renderer.h"
#include "Smile/Scene/CookedFormat.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
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

    bool Renderer::LoadCookedScene(const std::wstring& _ScenePath) {
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

        Scene.Clear();
        for (auto& m : ImportedMaterials) m->Release(SRVHeap);
        ImportedMaterials.clear();
        for (auto& t : ImportedTextures) t->Release(SRVHeap);
        ImportedTextures.clear();

        std::unordered_map<std::string, bool> uniquePaths; 
        auto consider = [&](const char* rel, bool srgb) {
            if (rel && rel[0]) uniquePaths.emplace(std::string(rel), srgb);
        };
        for (u32 i = 0; i < sh.MaterialCount; ++i) {
            consider(mats[i].BaseColor, true);
            consider(mats[i].Emissive, true);
            consider(mats[i].Specular, false);
            consider(mats[i].Normal,   false);
        }

        std::vector<std::string> relList;
        std::vector<bool>        srgbList;
        relList.reserve(uniquePaths.size());
        for (auto& kv : uniquePaths) { relList.push_back(kv.first); srgbList.push_back(kv.second); }

        std::vector<FTextureCPUData> cpuData(relList.size());
        {
            unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            unsigned nthreads = std::min<unsigned>(hw, static_cast<unsigned>(relList.size()));
            auto worker = [&](unsigned tid) {
                for (size_t i = tid; i < relList.size(); i += nthreads) {
                    std::wstring full = (sceneDir / fs::path(relList[i])).wstring();
                    cpuData[i] = FTexture::LoadDDSCPU(full, srgbList[i]);
                }
            };
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker, t);
            for (auto& th : pool) th.join();
        }
        const Clock::time_point tDecodeEnd = Clock::now();
        const double msDecode = MsSince(t0) - msRead;

        std::vector<FTexture> texs =
            FTexture::CreateBatchFromCPU(Device.Native(), CommandQueue, SRVHeap, cpuData);
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

            FTexture* baseT = getTex(sm.BaseColor);
            FTexture* specT = getTex(sm.Specular);
            FTexture* normT = getTex(sm.Normal);
            FTexture* emisT = getTex(sm.Emissive);

            mat->Albedo            = baseT ? baseT : &TexDefaultWhite;
            mat->Normal            = normT ? normT : &TexDefaultNormal;
            mat->MetallicRoughness = specT ? specT : &TexDefaultWhite;
            mat->AO                = &TexDefaultWhite;
            mat->Emissive          = emisT ? emisT : &TexDefaultBlack;
            mat->Height            = &TexDefaultWhite;
            mat->Metalness         = &TexDefaultWhite;
            mat->Roughness         = &TexDefaultWhite;

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
            mat->Constants.HasMetalnessMap         = 0u;
            mat->Constants.HasRoughnessMap         = 0u;

            mat->Constants.SpecularPacking    = specT ? 1u : 0u; 
            mat->Constants.MetallicFactor     = specT ? 1.0f : 0.0f;
            mat->Constants.RoughnessFactor    = specT ? 1.0f : 0.8f;

            mat->Constants.AOStrength         = 0.0f;
            mat->Constants.NormalFlipY        = 1u;              
            mat->Constants.NormalReconstructZ =
                (normT && normT->Format() == DXGI_FORMAT_BC5_UNORM) ? 1u : 0u;
            mat->Constants.AlphaTest          = sm.AlphaTest;
            mat->Constants.AlphaCutoff        = sm.AlphaCutoff;
            mat->TwoSided                     = (sm.TwoSided != 0);

            const bool isFoliage = (sm.AlphaTest != 0u) && (sm.TwoSided != 0);
            mat->Constants.ShadingModel   = isFoliage ? 1u : 0u;
            mat->Constants.SubsurfaceColor = { 1.0f, 1.0f, 1.0f, isFoliage ? 0.6f : 0.0f };

            mat->UpdateConstants();
            matPtrs[i] = mat.get();
            ImportedMaterials.push_back(std::move(mat));
        }

        std::vector<SMeshEntry> entries(mh.MeshCount);
        f32 aabbMin[3] = {  1e30f,  1e30f,  1e30f };
        f32 aabbMax[3] = { -1e30f, -1e30f, -1e30f };
        for (u32 i = 0; i < mh.MeshCount; ++i) {
            std::memcpy(&entries[i], entriesRaw + sizeof(SMeshEntry) * i, sizeof(SMeshEntry));
            for (int c = 0; c < 3; ++c) {
                aabbMin[c] = std::min(aabbMin[c], entries[i].AABBMin[c]);
                aabbMax[c] = std::max(aabbMax[c], entries[i].AABBMax[c]);
            }
        }
        auto matOf = [&](u32 mi) -> FMaterial* {
            return (mi != kNoMaterial && mi < sh.MaterialCount) ? matPtrs[mi] : nullptr;
        };

        const Clock::time_point tMeshStart = Clock::now();
        if (!MergeByMaterial) {
            std::vector<FMesh> meshesCPU(mh.MeshCount);
            for (u32 i = 0; i < mh.MeshCount; ++i) {
                const SMeshEntry& e = entries[i];
                meshesCPU[i].Vertices.resize(e.VertexCount);
                std::memcpy(meshesCPU[i].Vertices.data(), geoBase + e.VertexOffset, e.VertexCount * sizeof(Vertex));
                meshesCPU[i].Indices.resize(e.IndexCount);
                std::memcpy(meshesCPU[i].Indices.data(), geoBase + e.IndexOffset, e.IndexCount * sizeof(u32));
            }
            std::vector<FGpuMesh*> meshPtrs = Scene.AddMeshesBatch(Device.Native(), CommandQueue, meshesCPU);
            if (sh.RenderableCount > MaxObjects) { MaxObjects = sh.RenderableCount; RecreateObjectCB(); }
            for (u32 i = 0; i < sh.RenderableCount; ++i) {
                const SSceneRenderable& r = rnds[i];
                if (r.MeshIndex >= mh.MeshCount) continue;
                FRenderable out;
                out.Mesh     = meshPtrs[r.MeshIndex];
                out.Material = matOf(r.MaterialIndex);
                const SMeshEntry& e = entries[r.MeshIndex];
                out.AABBMin = Vec3{ e.AABBMin[0], e.AABBMin[1], e.AABBMin[2] };
                out.AABBMax = Vec3{ e.AABBMax[0], e.AABBMax[1], e.AABBMax[2] };
                Scene.AddRenderable(out);
            }
        } else {
            std::unordered_map<u32, std::vector<u32>> groups; 
            groups.reserve(sh.MaterialCount + 1);
            for (u32 i = 0; i < sh.RenderableCount; ++i) {
                if (rnds[i].MeshIndex >= mh.MeshCount) continue;
                groups[rnds[i].MaterialIndex].push_back(rnds[i].MeshIndex);
            }
            std::vector<FMesh> meshesCPU;  meshesCPU.reserve(groups.size());
            std::vector<u32>   groupMat;   groupMat.reserve(groups.size());
            std::vector<Vec3>  gMin, gMax; gMin.reserve(groups.size()); gMax.reserve(groups.size());
            for (auto& g : groups) {
                FMesh m;
                f32 mn[3] = {  1e30f,  1e30f,  1e30f };
                f32 mx[3] = { -1e30f, -1e30f, -1e30f };
                for (u32 ei : g.second) {
                    const SMeshEntry& e = entries[ei];
                    const u32 base = static_cast<u32>(m.Vertices.size());
                    const Vertex* vsrc = reinterpret_cast<const Vertex*>(geoBase + e.VertexOffset);
                    m.Vertices.insert(m.Vertices.end(), vsrc, vsrc + e.VertexCount);
                    const u32* isrc = reinterpret_cast<const u32*>(geoBase + e.IndexOffset);
                    m.Indices.reserve(m.Indices.size() + e.IndexCount);
                    for (u32 k = 0; k < e.IndexCount; ++k) m.Indices.push_back(isrc[k] + base);
                    for (int c = 0; c < 3; ++c) { mn[c] = std::min(mn[c], e.AABBMin[c]); mx[c] = std::max(mx[c], e.AABBMax[c]); }
                }
                meshesCPU.push_back(std::move(m));
                groupMat.push_back(g.first);
                gMin.push_back(Vec3{ mn[0], mn[1], mn[2] });
                gMax.push_back(Vec3{ mx[0], mx[1], mx[2] });
            }
            if (static_cast<u32>(meshesCPU.size()) > MaxObjects) {
                MaxObjects = static_cast<u32>(meshesCPU.size()); RecreateObjectCB();
            }
            std::vector<FGpuMesh*> meshPtrs = Scene.AddMeshesBatch(Device.Native(), CommandQueue, meshesCPU);
            for (size_t k = 0; k < meshPtrs.size(); ++k) {
                FRenderable out;
                out.Mesh     = meshPtrs[k];
                out.Material = matOf(groupMat[k]);
                out.AABBMin  = gMin[k];
                out.AABBMax  = gMax[k];
                Scene.AddRenderable(out);
            }
        }

        const double msMesh = MsSince(tMeshStart);

        Camera.SetPose(Vec3{ -14.476486f, 3.932823f, 0.278743f }, -9.05f, 78.75f);

        LogInfo("Cena carregada: " + std::to_string(mh.MeshCount) + " meshes, " +
                std::to_string(sh.MaterialCount) + " materiais, " +
                std::to_string(sh.RenderableCount) + " renderaveis, " +
                std::to_string(uploaded) + " texturas DDS.");
        LogInfo("Load (ms): leitura=" + std::to_string((int)msRead) +
                " decode=" + std::to_string((int)msDecode) +
                " uploadTex=" + std::to_string((int)msTexUpload) +
                " meshes=" + std::to_string((int)msMesh) +
                " | total=" + std::to_string((int)MsSince(t0)));

        BuildRaytracingScene();
        SetupGIForScene(Vec3{ aabbMin[0], aabbMin[1], aabbMin[2] },
                        Vec3{ aabbMax[0], aabbMax[1], aabbMax[2] });
        return true;
    }
}
