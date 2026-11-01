#include "Smile/Graphics/RayTracing/OmmBakeCache.h"

#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Graphics/Resources/Texture.h"
#include "Smile/Scene/SceneLoader.h"

#include <omm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <tuple>

namespace Smile {
    u32 OmmSdkVersion() {
        return (static_cast<u32>(OMM_VERSION_MAJOR) << 16) |
               (static_cast<u32>(OMM_VERSION_MINOR) << 8) |
               static_cast<u32>(OMM_VERSION_BUILD);
    }

    namespace {
        u32 BitsOf(f32 _V) { u32 B; std::memcpy(&B, &_V, sizeof(B)); return B; }
        f32 FloatOf(u32 _B) { f32 V; std::memcpy(&V, &_B, sizeof(V)); return V; }

        // Layout persistido do .somm. Tudo POD de tamanho fixo, conferido por static_assert: o
        // arquivo e lido por memcpy, e um campo a mais sem bump de kFormatVersion leria lixo
        // plausivel.
        //
        //   [SOmmFileHeader]
        //   EntryCount x { [SOmmEntry] [SOmmDesc x DescCount] [SOmmHistogram x HistogramCount]
        //                  [ArrayData: ArrayBytes] [Indices: TriangleCount * IndexStride] }
        //
        // PayloadHash cobre tudo depois do cabecalho: arquivo truncado ou corrompido e recusado
        // inteiro, antes de qualquer entrada virar micro-map.
        struct SOmmFileHeader {
            u32 Magic;
            u32 FormatVersion;
            u32 SettingsVersion; // kOmmBakeSettingsVersion
            u32 SdkVersion;      // OmmSdkVersion()
            u32 EntryCount;
            u32 BakerFlavor;     // FOmmBakeCache::CurrentFlavor() de quem gravou — informativo
            u64 PayloadHash;
        };
        static_assert(sizeof(SOmmFileHeader) == 32, "SOmmFileHeader e formato persistido");

        struct SOmmEntry {
            u32 MeshIndex;
            u32 MaterialIndex;
            u64 GeometryHash;
            u64 TextureHash;
            u32 BaseAlphaBits;
            u32 CutoffBits;
            u32 TriangleCount;
            u32 IndexStride;
            u32 IndexFormat;   // DXGI_FORMAT
            u32 DescCount;
            u32 HistogramCount;
            u32 WorkloadExcluded;
            u32 FullyKnown;
            u32 Micromap;
            u32 FullyUnknown;
            f32 KnownArea;
            u64 ArrayBytes;
        };
        static_assert(sizeof(SOmmEntry) == 80, "SOmmEntry e formato persistido");

        // Os campos do D3D12_RAYTRACING_OPACITY_MICROMAP_DESC sao bitfields: o layout do struct
        // nativo e do compilador, nao do arquivo. Por isso a copia campo a campo.
        struct SOmmDesc {
            u32 ByteOffset;
            u16 SubdivisionLevel;
            u16 Format;
        };
        static_assert(sizeof(SOmmDesc) == 8, "SOmmDesc e formato persistido");

        struct SOmmHistogram {
            u32 Count;
            u32 SubdivisionLevel;
            u32 Format;
        };
        static_assert(sizeof(SOmmHistogram) == 12, "SOmmHistogram e formato persistido");

        bool ValidFormat(u32 _F) {
            return _F == D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE ||
                   _F == D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
        }

        template <class T> void Append(std::vector<u8>& _Out, const T& _V) {
            const auto* P = reinterpret_cast<const u8*>(&_V);
            _Out.insert(_Out.end(), P, P + sizeof(T));
        }

        // Leitor com limites: cada Read confere o que resta ANTES de copiar. Um EntryCount ou um
        // ArrayBytes corrompido nunca vira alocacao gigante nem leitura fora do buffer.
        struct FReader {
            const u8* Data = nullptr;
            size_t    Size = 0;
            size_t    Pos  = 0;
            bool Read(void* _Dst, size_t _Bytes) {
                if (_Bytes > Size - Pos) return false;
                std::memcpy(_Dst, Data + Pos, _Bytes);
                Pos += _Bytes;
                return true;
            }
            bool Fits(u64 _Count, u64 _Stride) const {
                return _Stride == 0 || _Count <= (Size - Pos) / _Stride;
            }
        };
    }

    std::vector<FOmmBakeJob> CollectOmmBakeJobs(const FSceneImportResult& _Scene) {
        std::unordered_map<std::string, const FTextureCPUData*> TextureByPath;
        for (size_t I = 0; I < _Scene.TexturePaths.size() && I < _Scene.TextureData.size(); ++I)
            TextureByPath[_Scene.TexturePaths[I]] = &_Scene.TextureData[I];
        // Um atlas de folhagem serve dezenas de pares: hashear uma vez por textura.
        std::unordered_map<const FTextureCPUData*, FOmmContentHash> TextureHash;
        auto HashOf = [&](const FTextureCPUData* _T) -> FOmmContentHash {
            // O mesmo criterio do ExtractAlpha: nula ou sem mip e plano branco.
            if (!_T || !_T->Valid()) return kOmmWhiteTexture;
            const auto [It, Inserted] = TextureHash.try_emplace(_T, kOmmNoContentHash);
            if (Inserted) It->second = OmmHashTexture(*_T);
            return It->second;
        };

        std::vector<FOmmBakeJob> Jobs;
        std::unordered_map<u64, size_t> Seen;
        const u32 MeshCount = std::min<u32>(_Scene.MeshHeader.MeshCount,
                                            static_cast<u32>(_Scene.Meshes.size()));
        for (const SSceneRenderable& R : _Scene.Renderables) {
            if (R.MeshIndex >= MeshCount) continue;
            if (R.MaterialIndex == kNoMaterial || R.MaterialIndex >= _Scene.Materials.size())
                continue;
            const SSceneMaterial& M = _Scene.Materials[R.MaterialIndex];
            if (!M.AlphaTest) continue;
            const u64 Pair = (static_cast<u64>(R.MeshIndex) << 32) | R.MaterialIndex;
            if (!Seen.emplace(Pair, Jobs.size()).second) continue;

            FOmmBakeJob J;
            J.Mesh        = &_Scene.Meshes[R.MeshIndex];
            J.BaseAlpha   = M.BaseColorFactor[3];
            J.AlphaCutoff = M.AlphaCutoff;
            if (M.BaseColor[0]) {
                const auto It = TextureByPath.find(
                    std::string(M.BaseColor, strnlen(M.BaseColor, kCookedMaxPath)));
                if (It != TextureByPath.end()) J.AlphaTexture = It->second;
            }
            J.Key.MeshIndex     = R.MeshIndex;
            J.Key.MaterialIndex = R.MaterialIndex;
            J.Key.Texture       = HashOf(J.AlphaTexture);
            J.Key.BaseAlphaBits = BitsOf(J.BaseAlpha);
            J.Key.CutoffBits    = BitsOf(J.AlphaCutoff);
            if (!J.Mesh->Vertices.empty() && !J.Mesh->Indices.empty())
                J.Key.Geometry = OmmHashGeometry(
                    &J.Mesh->Vertices.front().TexCoord[0], sizeof(Vertex),
                    static_cast<u32>(J.Mesh->Vertices.size()), J.Mesh->Indices.data(),
                    static_cast<u32>(J.Mesh->Indices.size()));
            Jobs.push_back(J);
        }

        // Comecar pelas geometrias maiores reduz a cauda: os ultimos workers nao ficam presos
        // sozinhos nos bakes caros depois que todos os jobs pequenos ja terminaram.
        std::stable_sort(Jobs.begin(), Jobs.end(), [](const FOmmBakeJob& A, const FOmmBakeJob& B) {
            return A.Mesh->Indices.size() > B.Mesh->Indices.size();
        });
        return Jobs;
    }

    FOmmBakeRunStats BakeOmmJobs(const std::vector<FOmmBakeJob>& _Jobs,
                                 const std::vector<size_t>& _Which, u64 _MaxWorkloadSize,
                                 std::vector<std::shared_ptr<FOpacityMicromapData>>& _Results) {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point Start = Clock::now();
        FOmmBakeRunStats Stats;
        if (_Results.size() != _Jobs.size())
            throw std::logic_error("BakeOmmJobs: Results precisa de uma entrada por job");

        // OMM possui dois perfis bem diferentes: geometrias grandes/UVs patologicas escalam com
        // o OpenMP interno, enquanto muitos jobs pequenos ganham mais sendo executados em
        // paralelo sem equipes aninhadas. Separar as duas faixas evita uma cauda single-thread.
        const unsigned HardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        const unsigned WorkerBudget = HardwareThreads > 2 ? HardwareThreads - 2 : 1;

        // ALPHA PREPARADO uma vez por (textura, alpha, cutoff), e nao por par: era 70% do tempo
        // de bake no Bistro (ver FOmmPreparedAlpha). As preparacoes sao independentes entre si e
        // rodam em paralelo; os bakes de depois so leem o preparado.
        using FAlphaKey = std::tuple<const FTextureCPUData*, u32, u32>;
        auto KeyOf = [](const FOmmBakeJob& J) {
            return FAlphaKey{ J.AlphaTexture, J.Key.BaseAlphaBits, J.Key.CutoffBits };
        };
        std::map<FAlphaKey, std::shared_ptr<FOmmPreparedAlpha>> Prepared;
        std::map<FAlphaKey, const FOmmBakeJob*> PrepareFrom;
        for (size_t I : _Which) PrepareFrom.try_emplace(KeyOf(_Jobs[I]), &_Jobs[I]);
        {
            std::vector<std::pair<FAlphaKey, const FOmmBakeJob*>> Todo(PrepareFrom.begin(),
                                                                     PrepareFrom.end());
            std::vector<std::shared_ptr<FOmmPreparedAlpha>> Made(Todo.size());
            std::atomic_size_t Next{ 0 };
            std::mutex ErrorMutex;
            std::exception_ptr Error;
            auto Prepare = [&]() {
                for (size_t K; (K = Next.fetch_add(1)) < Todo.size();) {
                    try {
                        const FOmmBakeJob& J = *Todo[K].second;
                        Made[K] = FOmmPreparedAlpha::Create(J.AlphaTexture, J.BaseAlpha, J.AlphaCutoff);
                    } catch (...) {
                        std::scoped_lock Lock(ErrorMutex);
                        if (!Error) Error = std::current_exception();
                    }
                }
            };
            std::vector<std::jthread> Threads;
            const unsigned Count = std::min<unsigned>(WorkerBudget, static_cast<unsigned>(Todo.size()));
            for (unsigned T = 0; T < Count; ++T) Threads.emplace_back(Prepare);
            for (std::jthread& T : Threads) T.join();
            if (Error) std::rethrow_exception(Error);
            for (size_t K = 0; K < Todo.size(); ++K) Prepared[Todo[K].first] = Made[K];
        }
        auto AlphaOf = [&](const FOmmBakeJob& J) -> const FOmmPreparedAlpha& {
            return *Prepared.at(KeyOf(J));
        };

        constexpr size_t kInternalThreadTriangleThreshold = 4096;
        std::vector<size_t> Parallel;
        Parallel.reserve(_Which.size());
        for (size_t I : _Which) {
            const FOmmBakeJob& Job = _Jobs[I];
            if (Job.Mesh->Indices.size() / 3 >= kInternalThreadTriangleThreshold) {
                _Results[I] = FOpacityMicromapBaker::Bake(*Job.Mesh, AlphaOf(Job),
                                                          true, WorkerBudget, _MaxWorkloadSize);
                ++Stats.InternalJobs;
            } else {
                Parallel.push_back(I);
            }
        }

        const unsigned WorkerCount = std::min<unsigned>(
            std::min(WorkerBudget, 8u), static_cast<unsigned>(Parallel.size()));
        std::atomic_size_t NextJob{ 0 };
        std::atomic_bool StopWorkers{ false };
        std::mutex ErrorMutex;
        std::exception_ptr FirstError;
        auto Worker = [&]() {
            while (!StopWorkers.load(std::memory_order_relaxed)) {
                const size_t Slot = NextJob.fetch_add(1, std::memory_order_relaxed);
                if (Slot >= Parallel.size()) return;
                const FOmmBakeJob& Job = _Jobs[Parallel[Slot]];
                try {
                    _Results[Parallel[Slot]] = FOpacityMicromapBaker::Bake(
                        *Job.Mesh, AlphaOf(Job), false, 0, _MaxWorkloadSize);
                } catch (...) {
                    {
                        std::scoped_lock Lock(ErrorMutex);
                        if (!FirstError) FirstError = std::current_exception();
                    }
                    StopWorkers.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        };
        std::vector<std::jthread> Workers;
        Workers.reserve(WorkerCount);
        for (unsigned I = 0; I < WorkerCount; ++I) Workers.emplace_back(Worker);
        for (std::jthread& W : Workers) W.join();
        if (FirstError) std::rethrow_exception(FirstError);

        Stats.ParallelJobs = static_cast<u32>(Parallel.size());
        Stats.Workers      = WorkerCount;
        Stats.Ms = std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
        return Stats;
    }

    u32 FOmmBakeCache::CurrentFlavor() {
#if defined(NDEBUG)
        return kFlavorOptimized;
#else
        return kFlavorDebug;
#endif
    }

    std::filesystem::path FOmmBakeCache::PathFor(const std::filesystem::path& _SceneBasePath) {
        std::filesystem::path P = _SceneBasePath;
        P += L".somm";
        return P;
    }

    void FOmmBakeCache::Put(const FOmmBakeKey& _Key, std::shared_ptr<FOpacityMicromapData> _Data) {
        Entries_[PairOf(_Key.MeshIndex, _Key.MaterialIndex)] = FEntry{ _Key, std::move(_Data) };
    }

    FOmmBakeCache::ELookup FOmmBakeCache::Lookup(const FOmmBakeKey& _Key,
                                                 std::shared_ptr<FOpacityMicromapData>& _Out) const {
        _Out.reset();
        const auto It = Entries_.find(PairOf(_Key.MeshIndex, _Key.MaterialIndex));
        if (It == Entries_.end()) return ELookup::Missing;
        if (!(It->second.Key == _Key)) return ELookup::Stale;
        _Out = It->second.Data;
        return ELookup::Hit;
    }

    bool FOmmBakeCache::Save(const std::filesystem::path& _Path, std::string& _WhyNot) const {
        std::vector<u8> Payload;
        // Ordem estavel no arquivo (o unordered_map nao tem): dois saves do mesmo cache dao os
        // mesmos bytes, e um diff de sidecar e legivel.
        std::vector<const FEntry*> Sorted;
        Sorted.reserve(Entries_.size());
        for (const auto& [Pair, E] : Entries_) Sorted.push_back(&E);
        std::sort(Sorted.begin(), Sorted.end(), [](const FEntry* A, const FEntry* B) {
            return PairOf(A->Key.MeshIndex, A->Key.MaterialIndex) <
                   PairOf(B->Key.MeshIndex, B->Key.MaterialIndex);
        });

        for (const FEntry* E : Sorted) {
            const FOpacityMicromapData& D = *E->Data;
            if (!D.Valid()) {
                _WhyNot = "entrada invalida no cache (mesh " + std::to_string(E->Key.MeshIndex) + ")";
                return false;
            }
            SOmmEntry H{};
            H.MeshIndex        = E->Key.MeshIndex;
            H.MaterialIndex    = E->Key.MaterialIndex;
            H.GeometryHash     = E->Key.Geometry;
            H.TextureHash      = E->Key.Texture;
            H.BaseAlphaBits    = E->Key.BaseAlphaBits;
            H.CutoffBits       = E->Key.CutoffBits;
            H.TriangleCount    = D.TriangleCount;
            H.IndexStride      = D.IndexStride;
            H.IndexFormat      = static_cast<u32>(D.IndexFormat);
            H.DescCount        = static_cast<u32>(D.Descs.size());
            H.HistogramCount   = static_cast<u32>(D.Histogram.size());
            H.WorkloadExcluded = D.WorkloadExcludedTriangles;
            H.FullyKnown       = D.Coverage.FullyKnownTriangles;
            H.Micromap         = D.Coverage.MicromapTriangles;
            H.FullyUnknown     = D.Coverage.FullyUnknownTriangles;
            H.KnownArea        = D.Coverage.KnownArea;
            H.ArrayBytes       = D.ArrayData.size();
            Append(Payload, H);
            for (const auto& Src : D.Descs)
                Append(Payload, SOmmDesc{ Src.ByteOffset, static_cast<u16>(Src.SubdivisionLevel),
                                          static_cast<u16>(Src.Format) });
            for (const auto& Src : D.Histogram)
                Append(Payload, SOmmHistogram{ Src.Count, Src.SubdivisionLevel,
                                               static_cast<u32>(Src.Format) });
            Payload.insert(Payload.end(), D.ArrayData.begin(), D.ArrayData.end());
            Payload.insert(Payload.end(), D.Indices.begin(), D.Indices.end());
        }

        SOmmFileHeader Header{};
        Header.Magic           = kMagic;
        Header.FormatVersion   = kFormatVersion;
        Header.SettingsVersion = kOmmBakeSettingsVersion;
        Header.SdkVersion      = OmmSdkVersion();
        Header.EntryCount      = static_cast<u32>(Sorted.size());
        Header.BakerFlavor     = CurrentFlavor();
        Header.PayloadHash     = OmmHashFinish(OmmHashAppend(kOmmHashSeed, Payload.data(),
                                                             Payload.size()));

        std::filesystem::path Tmp = _Path;
        Tmp += L".tmp";
        {
            std::ofstream F(Tmp, std::ios::binary | std::ios::trunc);
            if (!F) { _WhyNot = "nao consegui criar " + Tmp.string(); return false; }
            F.write(reinterpret_cast<const char*>(&Header), sizeof(Header));
            F.write(reinterpret_cast<const char*>(Payload.data()),
                    static_cast<std::streamsize>(Payload.size()));
            if (!F) { _WhyNot = "falha ao escrever " + Tmp.string(); return false; }
        }
        std::error_code Ec;
        std::filesystem::rename(Tmp, _Path, Ec);
        if (Ec) {
            _WhyNot = "nao consegui substituir " + _Path.string() + ": " + Ec.message();
            std::filesystem::remove(Tmp, Ec);
            return false;
        }
        return true;
    }

    bool FOmmBakeCache::Load(const std::filesystem::path& _Path, std::string& _WhyNot) {
        Entries_.clear();
        Flavor_ = 0;
        std::ifstream F(_Path, std::ios::binary | std::ios::ate);
        if (!F) { _WhyNot = "sidecar ausente"; return false; }
        const std::streamoff FileBytes = F.tellg();
        if (FileBytes < static_cast<std::streamoff>(sizeof(SOmmFileHeader))) {
            _WhyNot = "arquivo menor que o cabecalho";
            return false;
        }
        std::vector<u8> Bytes(static_cast<size_t>(FileBytes));
        F.seekg(0);
        F.read(reinterpret_cast<char*>(Bytes.data()), FileBytes);
        if (!F) { _WhyNot = "falha de leitura"; return false; }

        SOmmFileHeader Header;
        std::memcpy(&Header, Bytes.data(), sizeof(Header));
        if (Header.Magic != kMagic) { _WhyNot = "magic invalido"; return false; }
        if (Header.FormatVersion != kFormatVersion) {
            _WhyNot = "formato v" + std::to_string(Header.FormatVersion) + ", esperado v" +
                      std::to_string(kFormatVersion);
            return false;
        }
        if (Header.SettingsVersion != kOmmBakeSettingsVersion) {
            _WhyNot = "parametros de bake v" + std::to_string(Header.SettingsVersion) +
                      ", a engine usa v" + std::to_string(kOmmBakeSettingsVersion);
            return false;
        }
        if (Header.SdkVersion != OmmSdkVersion()) {
            _WhyNot = "bakeado com outro OMM SDK";
            return false;
        }
        const u8*    Payload      = Bytes.data() + sizeof(Header);
        const size_t PayloadBytes = Bytes.size() - sizeof(Header);
        if (OmmHashFinish(OmmHashAppend(kOmmHashSeed, Payload, PayloadBytes)) != Header.PayloadHash) {
            _WhyNot = "hash do payload nao confere (arquivo truncado ou corrompido)";
            return false;
        }

        FReader R{ Payload, PayloadBytes, 0 };
        std::unordered_map<u64, FEntry> Parsed;
        for (u32 E = 0; E < Header.EntryCount; ++E) {
            SOmmEntry H;
            if (!R.Read(&H, sizeof(H))) { _WhyNot = "entrada truncada"; return false; }
            const bool StrideOk =
                (H.IndexStride == 2 && H.IndexFormat == DXGI_FORMAT_R16_UINT) ||
                (H.IndexStride == 4 && H.IndexFormat == DXGI_FORMAT_R32_UINT);
            // Indice de 8 bits e recusado de proposito: o D3D12 nao tem caminho conferido para
            // ele (ver Docs/OMM-AUDIT.md), e o baker da engine nunca o produz.
            if (!StrideOk || H.TriangleCount == 0 || !R.Fits(H.DescCount, sizeof(SOmmDesc)) ||
                !R.Fits(H.HistogramCount, sizeof(SOmmHistogram))) {
                _WhyNot = "entrada com layout invalido (mesh " + std::to_string(H.MeshIndex) + ")";
                return false;
            }

            auto D = std::make_shared<FOpacityMicromapData>();
            D->TriangleCount             = H.TriangleCount;
            D->IndexStride               = H.IndexStride;
            D->IndexFormat               = static_cast<DXGI_FORMAT>(H.IndexFormat);
            D->BakedBaseAlpha            = FloatOf(H.BaseAlphaBits);
            D->BakedAlphaCutoff          = FloatOf(H.CutoffBits);
            D->WorkloadExcludedTriangles = H.WorkloadExcluded;
            D->Coverage.FullyKnownTriangles   = H.FullyKnown;
            D->Coverage.MicromapTriangles     = H.Micromap;
            D->Coverage.FullyUnknownTriangles = H.FullyUnknown;
            D->Coverage.KnownArea             = H.KnownArea;

            // Os Fits acima conferem cada contagem SOZINHA contra o que resta; somadas elas ainda
            // podem nao caber. Por isso todo Read confere o proprio retorno.
            D->Descs.resize(H.DescCount);
            for (u32 I = 0; I < H.DescCount; ++I) {
                SOmmDesc S;
                if (!R.Read(&S, sizeof(S))) { _WhyNot = "descritores truncados"; return false; }
                if (!ValidFormat(S.Format)) { _WhyNot = "formato de micro-map invalido"; return false; }
                D->Descs[I].ByteOffset       = S.ByteOffset;
                D->Descs[I].SubdivisionLevel = S.SubdivisionLevel;
                D->Descs[I].Format = static_cast<D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT>(S.Format);
            }
            D->Histogram.resize(H.HistogramCount);
            for (u32 I = 0; I < H.HistogramCount; ++I) {
                SOmmHistogram S;
                if (!R.Read(&S, sizeof(S))) { _WhyNot = "histograma truncado"; return false; }
                if (!ValidFormat(S.Format)) { _WhyNot = "formato de histograma invalido"; return false; }
                D->Histogram[I].Count            = S.Count;
                D->Histogram[I].SubdivisionLevel = S.SubdivisionLevel;
                D->Histogram[I].Format = static_cast<D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT>(S.Format);
            }
            if (!R.Fits(H.ArrayBytes, 1) ||
                !R.Fits(static_cast<u64>(H.TriangleCount) * H.IndexStride, 1)) {
                _WhyNot = "dados de micro-map truncados";
                return false;
            }
            D->ArrayData.resize(static_cast<size_t>(H.ArrayBytes));
            D->Indices.resize(static_cast<size_t>(H.TriangleCount) * H.IndexStride);
            if ((H.ArrayBytes && !R.Read(D->ArrayData.data(), D->ArrayData.size())) ||
                !R.Read(D->Indices.data(), D->Indices.size())) {
                _WhyNot = "dados de micro-map truncados";
                return false;
            }

            // A mesma regra que o consumidor aplica antes do build, e as contagens fechando com o
            // buffer: entrada que nao passaria la nao entra aqui.
            if (!D->Valid() ||
                static_cast<u64>(H.FullyKnown) + H.Micromap + H.FullyUnknown != H.TriangleCount) {
                _WhyNot = "entrada inconsistente (mesh " + std::to_string(H.MeshIndex) + ")";
                return false;
            }
            for (const auto& Desc : D->Descs)
                if (Desc.ByteOffset >= D->ArrayData.size()) {
                    _WhyNot = "micro-map aponta fora do array (mesh " + std::to_string(H.MeshIndex) + ")";
                    return false;
                }

            FOmmBakeKey Key;
            Key.MeshIndex     = H.MeshIndex;
            Key.MaterialIndex = H.MaterialIndex;
            Key.Geometry      = H.GeometryHash;
            Key.Texture       = H.TextureHash;
            Key.BaseAlphaBits = H.BaseAlphaBits;
            Key.CutoffBits    = H.CutoffBits;
            if (!Parsed.emplace(PairOf(H.MeshIndex, H.MaterialIndex), FEntry{ Key, std::move(D) }).second) {
                _WhyNot = "par (malha, material) duplicado";
                return false;
            }
        }
        if (R.Pos != R.Size) { _WhyNot = "bytes sobrando depois da ultima entrada"; return false; }
        Entries_ = std::move(Parsed);
        Flavor_  = Header.BakerFlavor;
        return true;
    }
}
