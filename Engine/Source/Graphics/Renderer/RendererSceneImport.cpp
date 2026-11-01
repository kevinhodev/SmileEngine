#include "Smile/Graphics/Renderer/Renderer.h"
#include "Smile/Graphics/Renderer/RendererCaptureState.h"
#include "Smile/Graphics/Renderer/RendererSceneState.h"
#include "Smile/Graphics/Backend/RenderBackend.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Renderer/RenderSettings.h"
#include "Smile/Graphics/RayTracing/OmmBakeCache.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Scene/SceneLoader.h"
#include "Smile/Graphics/Debug/VramTracker.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace Smile {
    namespace {
        using Clock = std::chrono::steady_clock;

        double MsSince(Clock::time_point Start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
        }
    }

    // Aloca o ObjectCB para a capacidade corrente. NAO drena e NAO mexe no HiZ: quem chama decide
    // as duas coisas, porque os dois usos sao diferentes — o load de cena drena e refaz tudo, a
    // troca de toggle drena e refaz so este buffer.
    //
    // Devolve false sem tocar em nada quando a alocacao falha. E o que torna a troca de toggle
    // TRANSACIONAL: o chamador que desliga o bindless precisa do buffer ANTES de efetivar, e uma
    // falha aqui tem de deixa-lo bindless em vez de sem caminho nenhum.
    //
    // `Try`, e nao o `CreateUploadBuffer` normal: aquele usa SMILE_HR na criacao E no Map, entao
    // a falha nunca chegava ao `if` abaixo — ela saia como excecao pelo setter do editor/MCP, e o
    // "recusa e preserva o estado" desta funcao existia so no comentario.
    bool Renderer::AllocateObjectCB() {
        static_assert(sizeof(ObjectConstants) % 256 == 0,
                      "o CB de objeto e indexado por sizeof(); root CBV exige 256-alinhado");

        const GpuResources::FUploadBuffer Upload = GpuResources::TryCreateUploadBuffer(
            Backend->Device.Native(), sizeof(ObjectConstants),
            FCommandQueue::kFramesInFlight * MaxObjects);
        if (!Upload.Resource || !Upload.Mapped) return false;

        if (ObjectCB && MappedObjectCB) { ObjectCB->Unmap(0, nullptr); MappedObjectCB = nullptr; }
        ObjectCB       = Upload.Resource;
        MappedObjectCB = Upload.Mapped;

        // O footprint sai do RECURSO, e uma vez: `GetResourceAllocationInfo` sobre o desc que o
        // device de fato criou devolve o tamanho COBRADO, alinhamento incluso — a mesma medida que
        // o VramTracker usa, senao os dois numeros nao se comparam.
        //
        // Antes isto era `kFramesInFlight * MaxObjects * sizeof(...)`, recalculado no getter. A
        // formula acerta a ordem de grandeza e serve para provar zero, mas ela descreve o que foi
        // PEDIDO; publicar isso sob o nome "reservado" promete uma medicao que nao foi feita.
        const D3D12_RESOURCE_DESC Desc = ObjectCB->GetDesc();
        ObjectCBBytes = Backend->Device.Native()
                            ->GetResourceAllocationInfo(0, 1, &Desc).SizeInBytes;
        return true;
    }

    // Devolve os bytes do ObjectCB. O chamador ja drenou: o buffer mora em upload heap e frames em
    // voo podem estar lendo dele.
    void Renderer::ReleaseObjectCB() {
        if (ObjectCB && MappedObjectCB) { ObjectCB->Unmap(0, nullptr); MappedObjectCB = nullptr; }
        ObjectCB.Reset();
        ObjectCBBytes = 0;
    }

    // A versao do caminho de FRAME, para quando o buffer passa a poder ser aposentado sem ninguem
    // ter drenado — um hot reload que reconstroi a PSO bindless que faltava, por exemplo. Sem este
    // caminho a reconciliacao seria unilateral: o buffer voltaria e nunca mais iria embora.
    //
    // O ticket e o CurrentFenceSet (ultimos valores SUBMETIDOS), que e a fronteira certa: qualquer
    // frame que ainda leia este buffer ja foi submetido antes desta linha.
    //
    // Esta funcao NAO PODE alocar, e a razao e o desfecho de uma falha: um `push_back` que
    // realocasse e lancasse destruiria o temporario com o recurso dentro — liberando memoria que a
    // GPU ainda pode estar lendo —, com `ObjectCB` ja movido e desmapeado. Perder o buffer por
    // falta de espaco para UM elemento de vetor e o pior desfecho possivel daqui.
    //
    // A capacidade e reservada no Initialize e o teto e ESTRUTURAL: aposentar exige `ObjectCB` nao
    // nulo, e depois de aposentar ele e nulo — so uma realocacao o traz de volta, e no caminho de
    // frame ela acontece no maximo uma vez por frame. A coleta roda TODO frame (antes do ponto de
    // aborto, de proposito), e nenhum ticket sobrevive a kFramesInFlight frames. Logo a fila nunca
    // passa de kFramesInFlight + 1.
    //
    // A guarda existe mesmo assim, e falha para o lado seguro: sem espaco, o buffer continua vivo,
    // mapeado e valido, e a proxima tentativa e no frame seguinte. Memoria a mais, nunca recurso
    // liberado embaixo da GPU.
    void Renderer::RetireObjectCB() {
        if (!ObjectCB) return;
        if (RetiredObjectCBs.size() >= RetiredObjectCBs.capacity()) {
            if (!ObjectCBRetireFailureLogged) {
                LogError("ObjectCB: fila de aposentados cheia (o teto e estrutural, entao isto e "
                         "sintoma); o buffer continua alocado e a aposentadoria sera tentada de novo");
                ObjectCBRetireFailureLogged = true;
            }
            return; // estado INTACTO: nada foi desmapeado nem movido
        }
        ObjectCBRetireFailureLogged = false;

        if (MappedObjectCB) { ObjectCB->Unmap(0, nullptr); MappedObjectCB = nullptr; }
        // COPIA o ComPtr, e so depois solta o nosso. Com capacidade garantida o push_back nao
        // realoca e nao lanca — e mesmo assim a copia mantem a invariante "o recurso pertence a
        // alguem em TODO ponto" valida linha por linha, e nao por argumento.
        RetiredObjectCBs.push_back({ ObjectCB, CurrentFenceSet(), ObjectCBBytes });
        ObjectCB.Reset();
        ObjectCBBytes = 0;
    }

    void Renderer::CollectRetiredObjectCBs() {
        if (RetiredObjectCBs.empty()) return;
        const FGpuFenceSet Completed = CompletedFenceSet();
        std::erase_if(RetiredObjectCBs, [&](const FRetiredObjectCB& Entry) {
            return Completed.Covers(Entry.Required);
        });
    }

    // As cinco condicoes que os passes avaliam, no MESMO lugar. Cada uma ja existia espalhada:
    //   - UseBindlessMaterials: os passes com material escolhem a PSO classica sem ele, e a
    //     classica le o CBV por draw (invariante transform => material, ver RenderSettings);
    //   - BindlessTransformsReady: as PSOs das SETE consumidoras, nao so as do main view;
    //   - InstanceTable/MaterialTable construidas: sem elas o Begin* recusa e o passe cai;
    //   - ViewMatricesCB: o b2 do caminho bindless; sem ele o BeginBindlessTransforms recusa.
    //
    // Faltando UMA, algum passe desenha pelo classico — e o buffer tem de estar la.
    bool Renderer::CanRetireObjectCB() const {
        return UseBindlessMaterials && BindlessTransformsReady() && InstanceTable.IsBuilt() &&
               MaterialTable.IsBuilt() && ViewMatricesCB;
    }

    // Bytes que o ObjectCB AINDA OCUPA: o recurso ativo mais os aposentados que esperam a fence.
    //
    // Os aposentados CONTAM. Eu havia escrito que nao, para o numero do gate nao oscilar por um
    // detalhe de ciclo de vida — e isso era exatamente o defeito: este e um gate de MEMORIA, e
    // memoria ainda retida e memoria ainda retida. Excluindo-os, o numero chegaria a zero antes da
    // fence, e continuaria zero PARA SEMPRE se a coleta quebrasse. Um gate que nao consegue ficar
    // vermelho quando a coleta para nao esta medindo a coleta.
    //
    // A oscilacao existe e e curta: so depois de uma aposentadoria fora de dreno, pelos
    // kFramesInFlight frames ate a coleta. A troca de toggle nao passa por ali — ela drena e libera
    // na hora —, entao a medicao da fase le zero limpo.
    u64 Renderer::ObjectCBReservedBytes() const {
        u64 Total = ObjectCB ? ObjectCBBytes : 0;
        for (const FRetiredObjectCB& Entry : RetiredObjectCBs) Total += Entry.Bytes;
        return Total;
    }

    void Renderer::RecreateObjectCB() {
        Backend->DirectQueue.Flush();

        // O ring de view do caminho bindless vem ANTES da decisao abaixo: ele e uma das condicoes
        // do CanRetireObjectCB, e cria-lo depois deixaria a primeira chamada decidindo com o
        // proprio pre-requisito ainda nulo. NAO e dimensionado por MaxObjects — sao
        // kFramesInFlight slices, e so. E essa a troca que a fase persegue.
        if (!ViewMatricesCB) {
            const GpuResources::FUploadBuffer ViewUpload = GpuResources::CreateUploadBuffer(
                Backend->Device.Native(), sizeof(FGpuViewMatricesSlice),
                FCommandQueue::kFramesInFlight);
            ViewMatricesCB       = ViewUpload.Resource;
            MappedViewMatricesCB = ViewUpload.Mapped;
        }

        // Com o caminho bindless ligado ninguem le este buffer, e alocar N x 256 B por objeto so
        // para nao usar e exatamente o custo que a fase existe para remover. A condicao e a MESMA
        // do setter do toggle porque as duas chamam CanRetireObjectCB — e nao porque as duas
        // repetem a mesma lista, que foi como a primeira versao errou.
        //
        // Aqui a falha ABORTA, e nao recusa: este e o caminho de load/setup, onde nao existe
        // estado anterior para preservar — seguir sem buffer entregaria um renderer que nao
        // desenha e nao reclama. E o comportamento que o `CreateUploadBuffer` direto tinha antes
        // de a alocacao virar sob demanda; a recusa e do SETTER, que tem bindless para onde
        // voltar.
        if (!ObjectCBMustExist()) ReleaseObjectCB();
        else if (!AllocateObjectCB()) {
            const std::string Bytes = std::to_string(
                static_cast<u64>(FCommandQueue::kFramesInFlight) * MaxObjects *
                sizeof(ObjectConstants));
            LogError("RecreateObjectCB: sem memoria para o ObjectCB (" + Bytes +
                     " B); o raster classico nao tem de onde ler transform");
            throw std::runtime_error("ObjectCB: alocacao de " + Bytes + " B recusada pelo device");
        }

        // O HiZ e dimensionado pela MESMA capacidade, e por isso nasceu aqui dentro — mas ele NAO
        // depende do ObjectCB. Separado desde a Fase 4: o buffer de objeto passou a ir e vir com
        // o toggle, e arrastar a tabela de bounds do HiZ nessa troca refaria descritores e
        // invalidaria o readback de oclusao por um motivo que nada tem a ver com ele.
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
            // A compute tambem: o Clear vai aposentar as residencias, e o ticket exige as DUAS
            // filas. Drenar so a direta deixaria o Collect abaixo sem poder liberar nada.
            Backend->ComputeQueue.WaitIdle();

            // Sequencia DRENADA do reload, e a ordem importa:
            //     Clear/RetireAll(required) -> Collect(completed) -> alocar a nova cena.
            // Alocar antes do Collect nao quebra identidade -- os handles antigos continuam
            // invalidos --, mas faz o pool DOBRAR: os slots velhos ainda estao em Retired, a
            // cena nova nao acha nada em Free e cria outro tanto. Numa cena de 1M isso e 1M de
            // slots vazando por reload.
            const FGpuFenceSet Required = CurrentFenceSet();
            SceneState->Scene.Clear(Required);
            // Depois dos dois drenos acima, o concluido alcancou o submetido: o Collect aqui
            // devolve tudo, e a cena nova reusa em vez de crescer.
            SceneState->Scene.GpuSlots().Collect(Required);
            // E os ranges de DESCRITOR aposentados por troca de textura no editor. Sem esta
            // linha eles ficavam presos ate o proximo RenderFrame — que so acontece DEPOIS de a
            // cena nova alocar. Com os dois drenos acima, o concluido ja alcancou o submetido:
            // todos eles sao liberaveis agora, e nao ha razao para a cena nova encontrar o heap
            // mais cheio do que ele esta.
            Backend->SRVHeap.CollectRetired(Required);
            // As entradas de bake retidas descrevem a cena que esta indo embora: os ponteiros de
            // malha e de textura morrem nas linhas seguintes.
            OmmBakeInputs.Clear();
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
        std::unordered_map<std::string, const FTextureCPUData*> texCpuByPath;
        // Quais caminhos sao BaseColor de algum material. So esses interessam ao rebake de
        // micro-map — o plano de alpha do bake sai do BaseColor —, e restringir a eles evita pagar
        // uma entrada de procedencia por normal map, AO e afins, que nenhum bake consulta.
        std::unordered_set<std::string> baseColorPaths;
        for (const SSceneMaterial& M : Imported.Materials)
            if (M.BaseColor[0]) baseColorPaths.insert(M.BaseColor);

        u32 uploaded = 0;
        for (size_t i = 0; i < relList.size(); ++i) {
            if (i < Imported.TextureData.size()) texCpuByPath[relList[i]] = &Imported.TextureData[i];
            if (!texs[i].IsValid()) { texByPath[relList[i]] = nullptr; continue; }
            auto up = std::make_unique<FTexture>(std::move(texs[i]));
            texByPath[relList[i]] = up.get();
            // Procedencia da textura, para o rebake de micro-map poder rele-la do disco. Guardada
            // AQUI porque e o unico ponto que ainda tem os dois lados: o ponteiro vivo e o caminho
            // de onde ele veio.
            //
            // O DADO decodificado vai junto: dele saem as flags de load e o hash dos pixels que
            // acabaram de subir para a GPU. O rebake recalcula o mesmo hash sobre a releitura;
            // divergiu, recusa.
            if (baseColorPaths.contains(relList[i]) && i < Imported.TextureData.size())
                OmmBakeInputs.RetainTexturePath(
                    up.get(), (Imported.SceneDir / std::filesystem::path(relList[i])).wstring(),
                    Imported.TextureData[i]);
            ImportedTextures.push_back(std::move(up));
            ++uploaded;
        }

        auto getTex = [&](const char* rel) -> FTexture* {
            if (!rel || !rel[0]) return nullptr;
            auto it = texByPath.find(rel);
            return (it != texByPath.end()) ? it->second : nullptr;
        };
        auto getTexCpu = [&](const char* rel) -> const FTextureCPUData* {
            if (!rel || !rel[0]) return nullptr;
            const auto it = texCpuByPath.find(rel);
            return (it != texCpuByPath.end()) ? it->second : nullptr;
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

        // Os pares mascarados, pela MESMA enumeracao do SmileOmmBaker: e isso que garante que as
        // chaves gravadas offline sejam as que este load procura (ver OmmBakeCache.h). Os valores
        // de alpha saem do SSceneMaterial exatamente como o FMaterial acima foi montado.
        const std::vector<FOmmBakeJob> ommJobs = CollectOmmBakeJobs(Imported);
        std::unordered_map<u64, size_t> ommJobByMeshMaterial;
        for (size_t I = 0; I < ommJobs.size(); ++I)
            ommJobByMeshMaterial.emplace((static_cast<u64>(ommJobs[I].Key.MeshIndex) << 32) |
                                             ommJobs[I].Key.MaterialIndex, I);

        // O SIDECAR primeiro: o bake offline, sem teto, que o SmileOmmBaker grava em <cena>.somm.
        // O que ele nao cobre — sidecar ausente, recusado inteiro ou par desatualizado — vai para o
        // bake de runtime, com teto, exatamente como antes de o sidecar existir. Nao e fallback
        // para geometria sem OMM: os dois caminhos produzem o mesmo tipo de micro-map.
        const Clock::time_point tOmmBakeStart = Clock::now();
        std::vector<std::shared_ptr<FOpacityMicromapData>> ommResults(ommJobs.size());
        std::vector<size_t> ommPending;
        u32 ommFromSidecar = 0, ommStale = 0, ommMissing = 0;
        if (!ommJobs.empty()) {
            const fs::path SidecarPath   = FOmmBakeCache::PathFor(Imported.BasePath);
            const bool     SidecarExists = fs::exists(SidecarPath);
            FOmmBakeCache  Sidecar;
            std::string    Why;
            const bool     SidecarOk = SidecarExists && Sidecar.Load(SidecarPath, Why);
            if (SidecarExists && !SidecarOk)
                LogWarning("[OMM] Sidecar " + SidecarPath.filename().string() + " recusado (" + Why +
                           "); todos os pares vao pelo bake de runtime. Rode SmileOmmBaker na cena");
            for (size_t I = 0; I < ommJobs.size(); ++I) {
                switch (Sidecar.Lookup(ommJobs[I].Key, ommResults[I])) {
                    case FOmmBakeCache::ELookup::Hit:     ++ommFromSidecar; break;
                    case FOmmBakeCache::ELookup::Stale:   ++ommStale;   ommPending.push_back(I); break;
                    case FOmmBakeCache::ELookup::Missing: ++ommMissing; ommPending.push_back(I); break;
                }
            }
            if (SidecarOk && (ommStale || ommMissing))
                LogWarning("[OMM] Sidecar desatualizado: " + std::to_string(ommStale) +
                           " pares mudaram e " + std::to_string(ommMissing) +
                           " faltam desde o bake offline; esses vao pelo bake de runtime, com teto. "
                           "Rode SmileOmmBaker na cena");
            else if (!SidecarExists)
                LogDebug("[OMM] Sem sidecar " + SidecarPath.filename().string() +
                         "; bake de runtime, com teto, em todos os pares (o SmileOmmBaker gera o "
                         "bake offline)");
        }
        const FOmmBakeRunStats ommRun =
            BakeOmmJobs(ommJobs, ommPending, kOmmRuntimeMaxWorkload, ommResults);
        const double msOmmBake = MsSince(tOmmBakeStart);
        if (!ommJobs.empty()) {
            // Quanto da geometria mascarada ficou SEM micro-map por causa do teto de workload. E
            // o numero que diz se o OMM cobre a folhagem da cena, e o que um force-2-state
            // transformaria em opaco solido.
            u64 TotalTriangles = 0, ExcludedTriangles = 0;
            u64 FullyKnown = 0, WithMicromap = 0, FullyUnknown = 0;
            // A area conhecida de cada malha e ponderada em UV, e UV nao se compara entre texturas.
            // Na cena ela entra ponderada por TRIANGULO: e uma media de "quanto cada malha resolve",
            // nao uma area.
            double KnownAreaByTriangle = 0.0;
            for (const auto& Omm : ommResults) {
                if (!Omm) continue;
                TotalTriangles      += Omm->TriangleCount;
                ExcludedTriangles   += Omm->WorkloadExcludedTriangles;
                FullyKnown          += Omm->Coverage.FullyKnownTriangles;
                WithMicromap        += Omm->Coverage.MicromapTriangles;
                FullyUnknown        += Omm->Coverage.FullyUnknownTriangles;
                KnownAreaByTriangle += static_cast<double>(Omm->Coverage.KnownArea) *
                                       Omm->TriangleCount;
            }
            auto Pct = [TotalTriangles](double _V) {
                return std::to_string(TotalTriangles
                    ? static_cast<int>(100.0 * _V / static_cast<double>(TotalTriangles) + 0.5) : 0) + "%";
            };
            LogDebug("[OMM] " + std::to_string(ommJobs.size()) + " pares: " +
                     std::to_string(ommFromSidecar) + " do sidecar, " +
                     std::to_string(ommPending.size()) + " bakeados em runtime (" +
                     std::to_string(ommRun.InternalJobs) + " grandes internos + " +
                     std::to_string(ommRun.ParallelJobs) + " pequenos em " +
                     std::to_string(ommRun.Workers) + " workers) em " +
                     std::to_string(static_cast<int>(msOmmBake)) + " ms | sem micro-map por "
                     "workload: " + std::to_string(ExcludedTriangles) + " de " +
                     std::to_string(TotalTriangles) + " triangulos");
            LogDebug("[OMM] Cobertura de " + std::to_string(TotalTriangles) +
                     " triangulos mascarados: " + Pct(static_cast<double>(FullyKnown)) +
                     " resolvidos inteiros pelo hardware, " + Pct(static_cast<double>(WithMicromap)) +
                     " com micro-map, " + Pct(static_cast<double>(FullyUnknown)) +
                     " UNKNOWN inteiros (alpha-test em todo candidato) | area de UV conhecida, "
                     "media por triangulo: " + Pct(KnownAreaByTriangle));
        }

        const Clock::time_point tMeshUploadStart = Clock::now();
        const auto MeshCreationBase = GpuResources::CreationStats();
        std::vector<FGpuMesh*> meshPtrs = SceneState->Scene.AddMeshesBatch(
            Backend->Device.Native(), Backend->UploadQueue, Imported.Meshes);

        // Procedencia de TODAS as malhas, e nao so das mascaradas.
        //
        // O rebake de micro-map precisa disto, e restringir as mascaradas era um bug: um material
        // OPACO no import que ganhe Alpha Test depois nao teria com que bakear, e a cena ficaria
        // alpha-tested sem micro-map — que e exatamente o estado em que o CollectInstances aborta.
        // Como o que se guarda e o par (arquivo, indice) e nao a geometria, reter todas custa
        // dezenas de bytes por malha.
        {
            const std::wstring MeshFile = (fs::path(Imported.BasePath) += L".smesh").wstring();
            // O fingerprint sai dos BYTES QUE O LOADER JA TEM em RAM — os mesmos que viraram os
            // buffers de GPU logo acima. Nao acrescenta leitura ao load: e uma passada sobre dado
            // residente.
            //
            // Um carimbo de arquivo nao serviria, e a razao e uma janela concreta: o loader le, o
            // import bakeia, e o carimbo so sairia aqui — um recook nesse intervalo deixaria os
            // buffers antigos na GPU e registraria a identidade do arquivo NOVO.
            for (u32 i = 0; i < mh.MeshCount && i < meshPtrs.size() && i < Imported.Meshes.size();
                 ++i) {
                const FMesh& Src = Imported.Meshes[i];
                if (Src.Vertices.empty() || Src.Indices.empty()) continue;
                OmmBakeInputs.RetainMeshSource(
                    meshPtrs[i], MeshFile, i,
                    OmmHashGeometry(&Src.Vertices.front().TexCoord[0], sizeof(Vertex),
                                    static_cast<u32>(Src.Vertices.size()), Src.Indices.data(),
                                    static_cast<u32>(Src.Indices.size())));
            }
        }

        for (u32 i = 0; i < sh.RenderableCount; ++i) {
            const SSceneRenderable& r = rnds[i];
            if (r.MeshIndex >= mh.MeshCount) continue;
            FRenderable out;
            out.Name     = nameOf(r, r.MaterialIndex, i);
            out.Mesh     = meshPtrs[r.MeshIndex];
            out.Material = matOf(r.MaterialIndex);
            if (out.Material && out.Material->Constants.AlphaTest) {
                const u64 OmmKey = (static_cast<u64>(r.MeshIndex) << 32) |
                                   static_cast<u64>(r.MaterialIndex);
                const auto It = ommJobByMeshMaterial.find(OmmKey);
                if (It == ommJobByMeshMaterial.end() || It->second >= ommResults.size())
                    throw std::runtime_error("Job OMM ausente para renderable alpha-tested");
                out.OpacityMicromap = ommResults[It->second];
                // Carimba as ENTRADAS do bake na forma que o runtime reconhece. Aqui, e nao no
                // baker: e este ponto que conhece a malha de GPU e o material, e o baker so viu o
                // FMesh de CPU e os pixels. O carimbo e idempotente — o mesmo micro-map e
                // compartilhado por todas as instancias do par (mesh, material), e as duas
                // entradas de identidade sao exatamente as que o par fixa.
                //
                // Sem ele, os consumidores recusariam o micro-map: chave vazia nao casa com chave
                // viva nenhuma. E deliberado — um caminho de criacao novo que esqueca de carimbar
                // falha alto em vez de rodar com micro-map de procedencia desconhecida.
                out.OpacityMicromap->BakedInputs = FOmmInputKey::From(out.Mesh, out.Material);
            }
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
        // A cobertura de OMM da cena nova decide se o 2-state do GI pode ficar ligado.
        RefreshOmmGI2State();
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
