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
#include <exception>
#include <limits>

namespace fs = std::filesystem;

namespace Smile {
    struct FPreparedCookedScene {
        fs::path BasePath;
        fs::path ScenePath;
        fs::path SceneDir;
        SSceneHeader SceneHeader{};
        SMeshHeader  MeshHeader{};
        std::vector<SSceneMaterial>   Materials;
        std::vector<SSceneRenderable> Renderables;
        std::vector<SMeshEntry>       MeshEntries;
        std::vector<std::string>      TexturePaths;
        std::vector<FTextureCPUData>  TextureData;
        std::vector<FMesh>            Meshes;
        double ReadMs    = 0.0;
        double DecodeMs  = 0.0;
        double MeshMs    = 0.0;
        double PrepareMs = 0.0;
    };


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

    FPreparedCookedScenePtr Renderer::PrepareCookedScene(const std::wstring& _ScenePath) {
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

            auto Prepared = std::make_shared<FPreparedCookedScene>();
            Prepared->BasePath  = Base;
            Prepared->ScenePath = ScenePath;
            Prepared->SceneDir  = Base.parent_path();
            std::memcpy(&Prepared->SceneHeader, SceneBytes.data(), sizeof(SSceneHeader));
            std::memcpy(&Prepared->MeshHeader, MeshBytes.data(), sizeof(SMeshHeader));
            const SSceneHeader& SceneHeader = Prepared->SceneHeader;
            const SMeshHeader& MeshHeader = Prepared->MeshHeader;
            if (SceneHeader.Magic != kSSceneMagic || SceneHeader.Version != kCookedVersion ||
                MeshHeader.Magic != kSMeshMagic || MeshHeader.Version != kCookedVersion) {
                LogError("LoadCookedScene: magic/versao invalidos");
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

            Prepared->Materials.resize(SceneHeader.MaterialCount);
            if (SceneHeader.MaterialCount > 0) {
                std::memcpy(Prepared->Materials.data(), SceneBytes.data() + MaterialsOffset,
                            sizeof(SSceneMaterial) * SceneHeader.MaterialCount);
            }
            Prepared->Renderables.resize(SceneHeader.RenderableCount);
            if (SceneHeader.RenderableCount > 0) {
                std::memcpy(Prepared->Renderables.data(), SceneBytes.data() + RenderablesOffset,
                            sizeof(SSceneRenderable) * SceneHeader.RenderableCount);
            }
            Prepared->MeshEntries.resize(MeshHeader.MeshCount);
            if (MeshHeader.MeshCount > 0) {
                std::memcpy(Prepared->MeshEntries.data(), MeshBytes.data() + EntriesOffset,
                            sizeof(SMeshEntry) * MeshHeader.MeshCount);
            }

            for (const SMeshEntry& Entry : Prepared->MeshEntries) {
                if (Entry.VertexOffset > std::numeric_limits<size_t>::max() ||
                    Entry.IndexOffset > std::numeric_limits<size_t>::max() ||
                    !ArrayFits(static_cast<size_t>(Entry.VertexOffset), Entry.VertexCount,
                               sizeof(Vertex), GeometryBytes) ||
                    !ArrayFits(static_cast<size_t>(Entry.IndexOffset), Entry.IndexCount,
                               sizeof(u32), GeometryBytes)) {
                    LogError("LoadCookedScene: blob de geometria truncado");
                    return {};
                }
            }
            Prepared->ReadMs = MsSince(t0);

            struct FTextureFlags { bool SRGB; bool IsNormal; };
            std::unordered_map<std::string, FTextureFlags> UniquePaths;
            auto Consider = [&](const char* Relative, bool SRGB, bool IsNormal) {
                if (!Relative || !Relative[0]) return;
                UniquePaths.emplace(
                    std::string(Relative, strnlen(Relative, kCookedMaxPath)),
                    FTextureFlags{ SRGB, IsNormal });
            };
            for (const SSceneMaterial& Material : Prepared->Materials) {
                Consider(Material.BaseColor, true,  false);
                Consider(Material.Emissive,  true,  false);
                Consider(Material.Specular,  false, false);
                Consider(Material.Normal,    false, true);
                Consider(Material.Metalness, false, false);
                Consider(Material.Roughness, false, false);
            }

            std::vector<FTextureFlags> TextureFlags;
            Prepared->TexturePaths.reserve(UniquePaths.size());
            TextureFlags.reserve(UniquePaths.size());
            for (const auto& [Path, Flags] : UniquePaths) {
                Prepared->TexturePaths.push_back(Path);
                TextureFlags.push_back(Flags);
            }
            Prepared->TextureData.resize(Prepared->TexturePaths.size());

            const Clock::time_point DecodeStart = Clock::now();
            const unsigned HardwareThreads = std::max(1u, std::thread::hardware_concurrency());
            // O caller ja ocupa um worker preparando meshes; deixa mais um core livre para
            // o event loop/render do Editor enquanto os decoders trabalham.
            const unsigned WorkerBudget = HardwareThreads > 2 ? HardwareThreads - 2 : 1;
            const unsigned WorkerCount = std::min<unsigned>(
                std::min(WorkerBudget, 8u), static_cast<unsigned>(Prepared->TexturePaths.size()));
            auto DecodeWorker = [&](unsigned WorkerIndex) {
                for (size_t I = WorkerIndex; I < Prepared->TexturePaths.size(); I += WorkerCount) {
                    try {
                        const fs::path Relative(Prepared->TexturePaths[I]);
                        const std::wstring FullPath = (Prepared->SceneDir / Relative).wstring();
                        std::string Extension = Relative.extension().string();
                        for (char& C : Extension) if (C >= 'A' && C <= 'Z') C += 32;
                        Prepared->TextureData[I] = (Extension == ".dds")
                            ? FTexture::LoadDDSCPU(FullPath, TextureFlags[I].SRGB)
                            : FTexture::LoadCPU(
                                FullPath, TextureFlags[I].IsNormal, TextureFlags[I].SRGB);
                    } catch (const std::exception& Error) {
                        LogError("LoadCookedScene: falha ao preparar textura " +
                                 Prepared->TexturePaths[I] + ": " + Error.what());
                    }
                }
            };
            std::vector<std::jthread> Workers;
            for (unsigned I = 0; I < WorkerCount; ++I) Workers.emplace_back(DecodeWorker, I);

            const Clock::time_point MeshStart = Clock::now();
            Prepared->Meshes.resize(MeshHeader.MeshCount);
            const u8* GeometryBase = MeshBytes.data() + GeometryOffset;
            for (u32 I = 0; I < MeshHeader.MeshCount; ++I) {
                const SMeshEntry& Entry = Prepared->MeshEntries[I];
                FMesh& Mesh = Prepared->Meshes[I];
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
            }
            Prepared->MeshMs = MsSince(MeshStart);

            for (std::jthread& Worker : Workers) Worker.join();
            Prepared->DecodeMs = MsSince(DecodeStart);
            Prepared->PrepareMs = MsSince(t0);
            LogDebug("Prepare scene CPU (ms): leitura=" + std::to_string((int)Prepared->ReadMs) +
                     " decode=" + std::to_string((int)Prepared->DecodeMs) +
                     " meshes=" + std::to_string((int)Prepared->MeshMs) +
                     " | total=" + std::to_string((int)Prepared->PrepareMs));
            return Prepared;
        } catch (const std::exception& Error) {
            LogError("LoadCookedScene: falha na preparacao CPU: " + std::string(Error.what()));
            return {};
        }
    }

    bool Renderer::LoadCookedScene(const std::wstring& _ScenePath, bool _Additive) {
        return CommitCookedScene(PrepareCookedScene(_ScenePath), _Additive);
    }

    bool Renderer::CommitCookedScene(FPreparedCookedScenePtr _Prepared, bool _Additive) {
        if (!_Prepared) return false;
        const Clock::time_point t0 = Clock::now();
        const FPreparedCookedScene& Prepared = *_Prepared;
        const fs::path& base = Prepared.BasePath;
        const fs::path& scenePath = Prepared.ScenePath;
        const fs::path& sceneDir = Prepared.SceneDir;
        const SSceneHeader& sh = Prepared.SceneHeader;
        const SMeshHeader& mh = Prepared.MeshHeader;
        const auto& mats = Prepared.Materials;
        const auto& rnds = Prepared.Renderables;
        const auto& entries = Prepared.MeshEntries;

        // Em carga aditiva preservamos meshes/materiais/texturas ja carregados; so
        // limpamos a cena anterior no modo de substituicao (padrao).
        if (!_Additive) {
            // Os frames em voo (kFramesInFlight) ainda referenciam o que vem abaixo: os
            // ID3D12Resource das texturas (FTexture::Release faz Reset() no recurso), os ranges do
            // SRV heap SHADER-VISIBLE (FMaterial::Release devolve a tabela, e a cena nova realoca o
            // mesmo range e sobrescreve os descritores) e os slots do pool de CBV (o memcpy do
            // Finalize cai no endereco que o root CBV do frame anterior aponta). Sem drenar a fila
            // antes, isso e use-after-free — e o editor chama LoadCookedScene direto do handler de
            // menu, com o timer de render ativo. Mesmo motivo do Flush em RecreateObjectCB.
            CommandQueue.Flush();

            Scene.Clear();
            for (auto& m : ImportedMaterials) m->Release(SRVHeap);
            ImportedMaterials.clear();
            for (auto& t : ImportedTextures) t->Release(SRVHeap);
            ImportedTextures.clear();
        }

        const Clock::time_point TextureUploadStart = Clock::now();
        std::vector<FTexture> texs = FTexture::CreateBatchFromCPU(
            Device.Native(), UploadQueue, SRVHeap, Prepared.TextureData);
        const double msTexUpload = MsSince(TextureUploadStart);
        const std::vector<std::string>& relList = Prepared.TexturePaths;
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

        // FNV-1a 64 sobre os bytes crus do registro cozido = identidade estavel do material
        // (ver FMaterial::Id). Hashear o struct inteiro e seguro aqui: SSceneMaterial nao tem
        // padding (1664B de char arrays, divisivel por 4, seguidos so de f32/u32 = 1724B) e o
        // Cooker o preenche a partir de `SSceneMaterial out{}` com SetStr fazendo memset antes
        // do strncpy — ou seja, todo byte e deterministico, inclusive o resto dos char arrays.
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

        const Clock::time_point tMeshUploadStart = Clock::now();
        std::vector<FGpuMesh*> meshPtrs = Scene.AddMeshesBatch(
            Device.Native(), UploadQueue, Prepared.Meshes);
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

        const double msMeshUpload = MsSince(tMeshUploadStart);

        // Todos os uploads (texturas + meshes) foram submetidos SEM bloquear na fila COPY;
        // espera aqui, uma unica vez, antes do primeiro consumo (BLAS/DDGI/frame leem VB e SRV).
        const Clock::time_point tSyncStart = Clock::now();
        UploadQueue.WaitIdle();
        const double msSync = MsSince(tSyncStart);

        const Clock::time_point ObjectSetupStart = Clock::now();
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
        const double msObjectSetup = MsSince(ObjectSetupStart);

        LogDebug("Assets da cena preparados: " + std::to_string(mh.MeshCount) + " meshes, " +
                 std::to_string(sh.MaterialCount) + " materiais, " +
                 std::to_string(sh.RenderableCount) + " renderaveis, " +
                 std::to_string(uploaded) + " texturas DDS");
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
        const Clock::time_point TerrainStart = Clock::now();
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

                            // Albedo do proxy: textura bakeada com o blend de 4 camadas quando o
                            // terreno tem camadas (ver FTerrain::BakeProxyAlbedo); sem elas, o
                            // raster tambem desenha cor chapada e o fator constante basta.
                            // A textura entra no ImportedTextures, que e liberado junto com o
                            // ImportedMaterials — o mesmo lifetime do material que aponta p/ ela.
                            FTexture* proxyAlbedo = nullptr;
                            FTextureCPUData proxyAlbedoCPU;
                            if (Terrain.TakeProxyAlbedoCPU(proxyAlbedoCPU)) {
                                auto tex = std::make_unique<FTexture>(
                                    FTexture::CreateFromCPU(Device.Native(), UploadQueue, SRVHeap,
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
                            // Com a textura bakeada o fator vira NEUTRO: o hit shading faz
                            // albedo = BaseColor * textura, e a cor ja esta na textura.
                            mat->Constants.BaseColorFactor = proxyAlbedo
                                ? Vec4{ 1.0f, 1.0f, 1.0f, 1.0f }
                                : Vec4{ 0.26f, 0.32f, 0.19f, 1.0f }; // cor media do vale
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
        const double msTerrain = MsSince(TerrainStart);

        // O volume de GI NAO inclui o terreno de proposito: um terreno de km esticaria o
        // grid de probes do DDGI. Fora do volume o shading cai no fallback de ambiente;
        // terreno no GI de verdade vem na F3 (BLAS proxy na TLAS).
        const Clock::time_point RaytracingStart = Clock::now();
        BuildRaytracingScene();
        const double msRaytracing = MsSince(RaytracingStart);
        const Clock::time_point GIStart = Clock::now();
        SetupGIForScene(sceneMin, sceneMax);
        const double msGI = MsSince(GIStart);

        // Luzes puntuais: a carga nao-aditiva limpou a cena (Scene.Clear); o EDITOR repovoa
        // pelo <cena>.lights.json (LightsBridge::OnSceneLoaded) e invalida a selecao de luz.
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
        const auto TotalMs = static_cast<long long>(Prepared.PrepareMs + CommitMs);
        LogInfo(std::string(_Additive ? "Cena adicionada" : "Cena carregada") + " em " +
                std::to_string(TotalMs) + " ms: " + scenePath.filename().string() + " | " +
                std::to_string(mh.MeshCount) + " meshes, " +
                std::to_string(sh.MaterialCount) + " materiais, " +
                std::to_string(sh.RenderableCount) + " renderaveis, " +
                std::to_string(uploaded) + " texturas");
        return true;
    }
}
