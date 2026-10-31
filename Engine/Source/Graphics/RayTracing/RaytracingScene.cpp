#include "Smile/Graphics/RayTracing/RaytracingScene.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/D3D12Device.h"
#include "Smile/Graphics/Debug/VramTracker.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Scene/CapturedIdentity.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h" // sizeof(Vertex): stride do SRV bindless de VB
#include "Smile/Graphics/RayTracing/RTMasks.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <string>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        UINT64 AlignAS(UINT64 _Value) {
            constexpr UINT64 A = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
            return (_Value + A - 1) & ~(A - 1);
        }

        ComPtr<ID3D12Resource> CreateUAVBuffer(ID3D12Device* _Device, UINT64 _Size,
                                               D3D12_RESOURCE_STATES _State) {
            return GpuResources::CreateBuffer(_Device, _Size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                              _State, EVramCategory::RaytracingAS);
        }

        void PushTransition(std::vector<D3D12_RESOURCE_BARRIER>& _Out, ID3D12Resource* _Res,
                            D3D12_RESOURCE_STATES _Before, D3D12_RESOURCE_STATES _After) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = _Res;
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = _Before;
            B.Transition.StateAfter  = _After;
            _Out.push_back(B);
        }

        void GlobalUAVBarrier(ID3D12GraphicsCommandList4* _CL) {
            D3D12_RESOURCE_BARRIER UAV{};
            UAV.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UAV.UAV.pResource = nullptr;
            _CL->ResourceBarrier(1, &UAV);
        }

        // O snapshot por instancia MUDOU DE CASA na Fase 2: era um struct local deste arquivo
        // (FRTInstanceGeo, 100 B, treze campos de material copiados por instancia) e virou
        // Smile::FGpuInstanceGeo, ao lado das outras tabelas canonicas. A montagem continua aqui,
        // porque quem preenche tem de ser o dono da correspondencia com a TLAS.
        using FRTInstanceGeo = FGpuInstanceGeo;
    }

    static_assert(FRaytracingScene::kInstanceSlots == FCommandQueue::kFramesInFlight,
                  "InstanceUpload versionado por frame em voo");

    void FRaytracingScene::Release(FTextureSRVHeap& _SRVHeap) {
        if (TlasSRVSlot_ != kInvalidSlot) { _SRVHeap.Free(TlasSRVSlot_, 1); TlasSRVSlot_ = kInvalidSlot; }
        if (InstanceGeoSRVSlot_ != kInvalidSlot) {
            _SRVHeap.Free(InstanceGeoSRVSlot_, 1);
            InstanceGeoSRVSlot_ = kInvalidSlot;
        }
        InstanceGeoBuf.Reset();
        InstanceGeoCount = 0;
        BlasPool.Reset();
        RebuiltBlas.clear();
        RetiredBlas_.clear();
        RebuiltBlasBytes_ = 0;
        RetiredBlasBytes_ = 0;
        BlasByMesh.clear();
        OmmArrays.clear();
        Tlas.Reset();
        TlasScratch.Reset();
        for (u32 s = 0; s < kInstanceSlots; ++s) {
            InstanceUpload[s].Reset();
            InstanceMapped[s] = nullptr;
        }
        InstanceCapacity_ = 0;
        TlasOrigin_     = {};
        Built          = false;
        InstanceCount_ = 0;
        BlasCount_     = 0;
    }

    void FRaytracingScene::ForgetBlas(const FGpuMesh* _Mesh, const FOpacityMicromapData* _Omm) {
        const auto MeshIt = BlasByMesh.find(_Mesh);
        if (MeshIt == BlasByMesh.end()) return;
        const auto AddressIt = MeshIt->second.find(_Omm);
        if (AddressIt == MeshIt->second.end()) return;

        // A partir do erase abaixo a operacao nao pode mais falhar: se a reserva lancar, a chave,
        // os contadores e o recurso continuam exatamente como estavam. A versao anterior apagava
        // a chave primeiro e fazia o push depois; um bad_alloc deixaria um BLAS vivo sem caminho
        // para encontra-lo nem aposenta-lo.
        auto RebuiltIt = std::find_if(RebuiltBlas.begin(), RebuiltBlas.end(),
            [&](const FRebuiltBlas& R) { return R.Mesh == _Mesh && R.Omm == _Omm; });
        if (RebuiltIt != RebuiltBlas.end())
            RetiredBlas_.reserve(RetiredBlas_.size() + 1);

        MeshIt->second.erase(AddressIt);
        // A CHAVE sai daqui — o ponteiro do micro-map morto —, que sem isto ficaria pendurada e um
        // bake futuro poderia reusar o mesmo endereco de heap.
        //
        // O RECURSO, quando o par tinha um proprio, sai do vivo e vai para o aposentado. Nao e
        // solto agora: a TLAS do frame anterior ainda guarda o endereco dele, e so para de guardar
        // quando for refeita. Ver ReclaimRetiredBlas. Um BLAS do POOL nao aparece aqui e nao tem o
        // que soltar — o pool e liberado inteiro na proxima carga.
        if (RebuiltIt != RebuiltBlas.end()) {
            RebuiltBlasBytes_ -= RebuiltIt->Bytes;
            RetiredBlasBytes_ += RebuiltIt->Bytes;
            RetiredBlas_.push_back(std::move(*RebuiltIt));
            RebuiltBlas.erase(RebuiltIt);
        }
    }

    u64 FRaytracingScene::ReclaimRetiredBlas() {
        const u64 Freed = RetiredBlasBytes_;
        RetiredBlas_.clear();
        RetiredBlasBytes_ = 0;
        return Freed;
    }

    u32 FRaytracingScene::EnsureBlasFor(
        FD3D12Device& _Device, FCommandQueue& _Queue,
        const std::vector<std::pair<const FGpuMesh*, const FOpacityMicromapData*>>& _Required) {
        if (_Required.empty()) return 0;
        if (!Built)
            throw std::runtime_error("Cena de ray tracing nao construida no rebake de BLAS");
        ID3D12Device5* Dev5 = _Device.Device5();
        if (!Dev5)
            throw std::runtime_error("ID3D12Device5 indisponivel no rebake de BLAS");

        // So o que ainda nao existe, e UMA VEZ por par.
        //
        // A deduplicacao aqui nao e otimizacao, e correcao: `_Required` chega com uma entrada por
        // RENDERAVEL, e a lista inteira e montada antes de o primeiro resultado ser publicado no
        // BlasByMesh — entao sem o conjunto abaixo, N instancias da mesma folhagem passariam
        // todas pelo filtro e construiriam N OMM arrays e N BLAS identicos. O custo escalaria por
        // instancia em vez de por par unico, que e o oposto do que a dedup do bake ja garante.
        //
        // Fica DENTRO do EnsureBlasFor, e nao a cargo de quem chama: e uma propriedade do que
        // significa "garantir que exista", e um chamador futuro nao deveria precisar saber disso.
        // A chave do conjunto e o PAR, com igualdade estrutural. A primeira versao misturava os
        // dois ponteiros num inteiro e o usava como identidade: uma colisao viraria igualdade e
        // descartaria um BLAS necessario — e fabricar um ponteiro a partir de um inteiro
        // arbitrario e implementation-defined por cima. Hash pode colidir; identidade nao pode.
        struct FPair {
            const FGpuMesh* Mesh;
            const FOpacityMicromapData* Omm;
            bool operator==(const FPair& O) const { return Mesh == O.Mesh && Omm == O.Omm; }
        };
        struct FPairHash {
            size_t operator()(const FPair& P) const {
                return std::hash<const void*>{}(P.Mesh) ^
                       (std::hash<const void*>{}(P.Omm) << 1);
            }
        };
        std::vector<std::pair<const FGpuMesh*, const FOpacityMicromapData*>> Missing;
        std::unordered_set<FPair, FPairHash> SeenPair;
        for (const auto& [Mesh, Omm] : _Required) {
            if (!Mesh || !Mesh->IsValid()) continue;
            const auto MeshIt = BlasByMesh.find(Mesh);
            if (MeshIt != BlasByMesh.end()) {
                const auto It = MeshIt->second.find(Omm);
                if (It != MeshIt->second.end() && It->second != 0) continue;
            }
            if (!SeenPair.insert(FPair{ Mesh, Omm }).second) continue;
            Missing.push_back({ Mesh, Omm });
        }
        if (Missing.empty()) return 0;

        // Command list e allocator PROPRIOS. Alem de este ser um caminho sincrono e raro, o
        // isolamento e parte da recuperacao: qualquer alocacao/prebuild abaixo pode lancar. Se a
        // list compartilhada da fila ficasse aberta no meio, o frame seguinte tentaria reseta-la
        // num estado que nao controla. A list local simplesmente morre sem ser submetida.
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> Allocator;
        SMILE_HR(Dev5->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&Allocator)));
        ComPtr<ID3D12GraphicsCommandList4> CL4;
        SMILE_HR(Dev5->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&CL4)));
        ID3D12GraphicsCommandList4* CL = CL4.Get();

        // Um scratch e um resultado POR PAR, e nao um pool suballocado como no Build: sao poucos
        // (o que uma edicao de material toca), e o pool existe para amortizar milhares.
        // Os uploads tambem precisam sobreviver ate ExecuteAndSync: a command list guarda seus
        // enderecos GPU, mas nao mantem uma referencia aos recursos que eles identificam.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> Scratches;
        std::vector<GpuResources::FUploadBuffer> UploadInputs;
        UploadInputs.reserve(Missing.size() * 3);
        // O que sera publicado DEPOIS de a fila sincronizar. Enquanto este laco roda, nada em
        // BlasByMesh, RebuiltBlas, OmmArrays ou nos contadores muda: qualquer par pode lancar (o
        // driver recusa um prebuild, uma alocacao falha), e o Close/ExecuteAndSync no fim tambem.
        // Publicar cedo registrava um endereco cuja build nunca foi executada, e o filtro la em
        // cima daria o par por pronto na tentativa seguinte.
        std::vector<FRebuiltBlas> Pending;
        Pending.reserve(Missing.size());
        for (const auto& [Mesh, Omm] : Missing) {
            D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC Tri{};
            Tri.VertexBuffer.StartAddress  = Mesh->VertexBufferGPUVA();
            Tri.VertexBuffer.StrideInBytes = Mesh->VertexStride();
            Tri.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
            Tri.VertexCount                = Mesh->VertexCount();
            Tri.IndexBuffer                = Mesh->IndexBufferGPUVA();
            Tri.IndexFormat                = DXGI_FORMAT_R32_UINT;
            Tri.IndexCount                 = Mesh->GetIndexCount();
            Tri.Transform3x4               = 0;

            D3D12_RAYTRACING_GEOMETRY_DESC Geo{};
            D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC Linkage{};
            GpuResources::FUploadBuffer OmmIndexInput, OmmArrayInput, OmmDescInput;
            D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC ArrayDesc{};
            Microsoft::WRL::ComPtr<ID3D12Resource> OmmArray;
            u64 OmmArrayBytes = 0;

            if (Omm) {
                OmmIndexInput = GpuResources::CreateUploadBuffer(
                    Dev5, static_cast<u64>(Omm->Indices.size()), 1, false);
                std::memcpy(OmmIndexInput.Mapped, Omm->Indices.data(), Omm->Indices.size());

                if (!Omm->Descs.empty()) {
                    OmmArrayInput = GpuResources::CreateUploadBuffer(
                        Dev5, static_cast<u64>(Omm->ArrayData.size()), 1, false);
                    OmmDescInput = GpuResources::CreateUploadBuffer(
                        Dev5, static_cast<u64>(Omm->Descs.size() * sizeof(Omm->Descs[0])), 1, false);
                    std::memcpy(OmmArrayInput.Mapped, Omm->ArrayData.data(), Omm->ArrayData.size());
                    std::memcpy(OmmDescInput.Mapped, Omm->Descs.data(),
                                Omm->Descs.size() * sizeof(Omm->Descs[0]));

                    ArrayDesc.NumOmmHistogramEntries = static_cast<UINT>(Omm->Histogram.size());
                    ArrayDesc.pOmmHistogram          = Omm->Histogram.data();
                    ArrayDesc.InputBuffer            = OmmArrayInput.Address(0);
                    ArrayDesc.PerOmmDescs = {
                        OmmDescInput.Address(0), sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)
                    };

                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS OmmInputs{};
                    OmmInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
                    OmmInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                    OmmInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                    OmmInputs.NumDescs = 1;
                    OmmInputs.pOpacityMicromapArrayDesc = &ArrayDesc;
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO OmmInfo{};
                    Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&OmmInputs, &OmmInfo);
                    if (OmmInfo.ResultDataMaxSizeInBytes == 0 || OmmInfo.ScratchDataSizeInBytes == 0)
                        throw std::runtime_error("Driver recusou o prebuild do OMM Array no rebake");

                    OmmArray = CreateUAVBuffer(Dev5, OmmInfo.ResultDataMaxSizeInBytes,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                    OmmArrayBytes = OmmInfo.ResultDataMaxSizeInBytes;
                    auto OmmScratch = CreateUAVBuffer(Dev5, OmmInfo.ScratchDataSizeInBytes,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC OmmBuild{};
                    OmmBuild.Inputs = OmmInputs;
                    OmmBuild.DestAccelerationStructureData    = OmmArray->GetGPUVirtualAddress();
                    OmmBuild.ScratchAccelerationStructureData = OmmScratch->GetGPUVirtualAddress();
                    CL->BuildRaytracingAccelerationStructure(&OmmBuild, 0, nullptr);
                    GlobalUAVBarrier(CL);
                    Scratches.push_back(OmmScratch);
                }

                Linkage.OpacityMicromapIndexBuffer  = { OmmIndexInput.Address(0), Omm->IndexStride };
                Linkage.OpacityMicromapIndexFormat  = Omm->IndexFormat;
                Linkage.OpacityMicromapBaseLocation = 0;
                Linkage.OpacityMicromapArray = OmmArray ? OmmArray->GetGPUVirtualAddress() : 0;
                Geo.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
                Geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                Geo.OmmTriangles.pTriangles  = &Tri;
                Geo.OmmTriangles.pOmmLinkage = &Linkage;
            } else {
                Geo.Type      = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                Geo.Flags     = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                Geo.Triangles = Tri;
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
            Inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            Inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            Inputs.NumDescs      = 1;
            Inputs.pGeometryDescs = &Geo;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info{};
            Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Info);
            if (Info.ResultDataMaxSizeInBytes == 0)
                throw std::runtime_error("Driver recusou o prebuild do BLAS no rebake");

            // SEM compaction, ao contrario do Build: compactar exige um readback do tamanho, e
            // portanto um segundo round-trip de fila. Sao poucos BLAS e o desperdicio e medido em
            // RebuiltBlasBytes.
            auto Result = CreateUAVBuffer(Dev5, Info.ResultDataMaxSizeInBytes,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            auto Scratch = CreateUAVBuffer(Dev5, Info.ScratchDataSizeInBytes,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BuildDesc{};
            BuildDesc.Inputs = Inputs;
            BuildDesc.DestAccelerationStructureData    = Result->GetGPUVirtualAddress();
            BuildDesc.ScratchAccelerationStructureData = Scratch->GetGPUVirtualAddress();
            CL->BuildRaytracingAccelerationStructure(&BuildDesc, 0, nullptr);
            GlobalUAVBarrier(CL);

            Pending.push_back(FRebuiltBlas{ Mesh, Omm, Result, OmmArray,
                                            Info.ResultDataMaxSizeInBytes + OmmArrayBytes });
            Scratches.push_back(Scratch);
            if (OmmIndexInput) UploadInputs.push_back(std::move(OmmIndexInput));
            if (OmmArrayInput) UploadInputs.push_back(std::move(OmmArrayInput));
            if (OmmDescInput)  UploadInputs.push_back(std::move(OmmDescInput));
        }

        // Monte o estado FINAL ainda antes da submissao. Copiar pode alocar e lancar; fazendo-o
        // com a list local e antes do Execute, a falha descarta tudo sem tocar o estado publicado.
        // Depois da espera so restam swaps noexcept e os contadores.
        auto PublishedBlasByMesh = BlasByMesh;
        auto PublishedRebuiltBlas = RebuiltBlas;
        PublishedRebuiltBlas.reserve(PublishedRebuiltBlas.size() + Pending.size());
        u64 PublishedBytes = RebuiltBlasBytes_;
        for (auto& P : Pending) {
            PublishedBlasByMesh[P.Mesh][P.Omm] = P.Blas->GetGPUVirtualAddress();
            PublishedBytes += P.Bytes;
            PublishedRebuiltBlas.push_back(std::move(P));
        }

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL };
        // Sincrono: uploads e scratches morrem no fim deste escopo, e a TLAS do proximo frame
        // vai referenciar os enderecos novos. Isto e caminho de edicao manual, ja drenado pelo
        // chamador — nao ha frame em voo para atrapalhar.
        _Queue.ExecuteAndSync(Lists, 1);

        // A PUBLICACAO, e so agora. As tres trocas sao noexcept: ou o estado anterior inteiro
        // continua visivel, ou o novo inteiro passa a ser. O invariante "endereco registrado
        // implica build executada" vale inclusive sob excecao de alocacao.
        const u32 BuiltCount = static_cast<u32>(Pending.size());
        BlasByMesh.swap(PublishedBlasByMesh);
        RebuiltBlas.swap(PublishedRebuiltBlas);
        RebuiltBlasBytes_ = PublishedBytes;

        BlasCount_ += BuiltCount;
        return BuiltCount;
    }

    void FRaytracingScene::CollectInstances(const FScene& _Scene, const Vec3& _TlasOrigin,
                                            std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& _Out) const {
        // O InstanceID viaja ate os passes temporais por um canal FLOAT da superficie, como
        // (id + 1). Acima de kMaxTemporalInstanceId ele deixa de round-tripar, e o desfecho nao e
        // erro: e o objeto VIZINHO, em silencio. Recusar aqui e o unico ponto em que isso ainda e
        // um diagnostico.
        //
        // O InstanceID do DXR tambem tem 24 bits, entao o mesmo teto vale para ele.
        if (_Scene.Renderables().size() > kMaxTemporalInstanceCount)
            throw std::runtime_error(
                "TLAS com mais renderaveis que o InstanceID consegue enderecar (" +
                std::to_string(_Scene.Renderables().size()) + " > " +
                std::to_string(kMaxTemporalInstanceCount) + ")");

        _Out.clear();
        _Out.reserve(_Scene.Renderables().size());
        u32 Idx = 0;
        for (const FRenderable& R : _Scene.Renderables()) {
            const u32 ThisIdx = Idx++;
            if (!_Scene.GpuSlots().IsAlive(R.RaytracingHandle())) continue;
            const bool AlphaTest = R.Material && R.Material->Constants.AlphaTest;
            const FOpacityMicromapData* Omm = AlphaTest ? R.OpacityMicromap.get() : nullptr;
            if (AlphaTest && (!Omm || !Omm->Valid()))
                throw std::runtime_error("Material alpha-tested sem OMM valido; nao existe fallback DXR");
            // TODAS as quatro entradas do bake, e nao so alpha/cutoff: ver FOmmInputKey. Um
            // micro-map obsoleto nao produz excecao no DXR — produz recorte errado em folhagem.
            if (AlphaTest && !Omm->MatchesInputs(FOmmInputKey::From(R.Mesh, R.Material)))
                throw std::runtime_error(
                    "OMM desatualizado: uma entrada do bake (malha, textura de alpha, alpha ou "
                    "cutoff) mudou e o rebake nao alcancou. O RebakeStaleOpacityMicromaps roda no "
                    "topo do frame, antes disto — chegar aqui significa que ele falhou e "
                    "logou por que");
            auto MeshIt = BlasByMesh.find(R.Mesh);
            if (MeshIt == BlasByMesh.end())
                throw std::runtime_error("BLAS ausente para mesh da TLAS");
            auto BlasIt = MeshIt->second.find(Omm);
            if (BlasIt == MeshIt->second.end() || BlasIt->second == 0)
                throw std::runtime_error(AlphaTest
                    ? "Material alpha-tested sem BLAS OMM; recarregue a cena apos editar o modo/cutoff"
                    : "BLAS opaco ausente para mesh da TLAS");

            D3D12_RAYTRACING_INSTANCE_DESC Inst{};

            // DXR recebe a instancia no espaco relativo a camera. O mundo autoral, bounds,
            // luzes, volumes e caches continuam absolutos; so a fronteira do RayQuery muda.
            // Subtrair antes da transposta e importante: a Smile guarda translacao em M[3].
            Mat44 Relative = R.Transform.Matrix();
            Relative.M[3][0] -= _TlasOrigin.X;
            Relative.M[3][1] -= _TlasOrigin.Y;
            Relative.M[3][2] -= _TlasOrigin.Z;
            const Mat44 T = Relative.GetTransposed();
            for (int Row = 0; Row < 3; ++Row)
                for (int Col = 0; Col < 4; ++Col)
                    Inst.Transform[Row][Col] = T.M[Row][Col];
            // Indice na tabela DESTE frame, nao identidade duravel. Mantem os buracos da
            // lista para casar com InstanceGeo e BuildIdentityTable; leitores atrasados
            // resolvem pela tabela capturada, nunca pela lista de outro frame.
            Inst.InstanceID                          = ThisIdx;
            Inst.InstanceContributionToHitGroupIndex = 0;
            Inst.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            // Categoria da instancia (kRTMask* em RTMasks.h). UM bit por instancia; quem escolhe
            // o que enxerga e o PASSE, pela mask do raio — modelo do Lumen.
            // Folhagem/alpha-test usa OMM 4-state: transparente/opaco e resolvido pelo hardware;
            // somente microtriangulos UNKNOWN chegam ao AlphaTestPass (SMILE_RT_PROCEED).
            // Translucido (Blend) fica numa categoria propria: o gather do GI nao o inclui, entao
            // o vidro deixa de ser parede opaca para a iluminacao indireta. Ele CONTINUA na TLAS —
            // as reflexoes ainda o enxergam, que era a razao de nao simplesmente removE-lo.
            const bool Blend     = R.Material && R.Material->Blend;
            Inst.InstanceMask = Blend ? kRTMaskTranslucent
                                      : (AlphaTest ? kRTMaskAlphaTest : kRTMaskOpaque);
            // Culling. A instance flag VENCE a ray flag na spec do DXR, entao enquanto isto saia
            // incondicionalmente TODO RAY_FLAG_CULL_BACK_FACING_TRIANGLES da engine era no-op — a
            // cena inteira era double-sided no RT enquanto o raster cullava. A flag agora descreve
            // so a GEOMETRIA; quem decide cullar e cada passe, pela ray flag.
            //
            // Criterio = FMaterial::IsTwoSidedForRT, que espelha o que o RASTER desenha two-sided:
            // o G-buffer escolhe PSOGBufferTwoSided por `TwoSided || AlphaTest`, e o passe forward
            // dos translucidos e cull NONE para todos. So assim as duas visoes da cena concordam
            // sobre o que e visivel pelo verso. No cozido atual `TwoSided` e `AlphaTest` saem da
            // mesma condicao (cutout, em Cooker/main.cpp) — na pratica isto isola
            // folhagem/cutouts/vidro e deixa arquitetura one-sided.
            //
            // DDGI nao e afetado: ele traca com RAY_FLAG_NONE de proposito, porque a deteccao de
            // "probe dentro de geometria" depende de ENXERGAR o backface (distancia assinada em
            // DDGITrace). Ray flag e por raio, instance flag e por instancia — por isso os dois
            // regimes convivem na mesma TLAS.
            if (R.Material && R.Material->IsTwoSidedForRT())
                Inst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            Inst.AccelerationStructure               = BlasIt->second;
            _Out.push_back(Inst);
        }
    }

    // Irma do CollectInstances: as duas percorrem a MESMA lista na MESMA ordem, e e essa ordem que
    // liga as duas — o InstanceID que a TLAS carrega e o indice deste vetor, e e por ele que o hit
    // de RT indexa o snapshot. Preencher aqui, e nao no dono do volume DDGI, e o que garante que
    // as duas nunca sejam construidas em momentos diferentes.
    void FRaytracingScene::FillInstanceGeo(const FScene& _Scene, const FGpuMaterialTable& _Materials,
                                           const FGpuMeshTable& _Meshes, u8* _Mapped,
                                           u32 _Count) const {
        const u32 MaterialSrv = _Materials.Srv();
        const u32 MeshSrv     = _Meshes.Srv();
        for (u32 i = 0; i < _Count; ++i) {
            const FRenderable& R = _Scene.Renderables()[i];
            FRTInstanceGeo g{};
            g.MaterialSrv = MaterialSrv;
            g.MaterialId  = _Materials.RowOf(R.Material);
            g.MeshSrv     = MeshSrv;
            g.MeshId      = _Meshes.RowOf(R.Mesh);
            // O payload por triangulo continua DESNORMALIZADO aqui (ver FGpuInstanceGeo): e o
            // caminho quente, e N instancias da mesma malha recebem o mesmo descritor — o que
            // torna o payload por malha e nao por instancia, e mantem a conta de VRAM em
            // triangulos unicos.
            //
            // O guard pela LINHA, e nao pelo ponteiro: sem ele uma malha desconhecida receberia
            // GeometrySlotOf()+2 = 2, um descritor plausivel de outro dono. Zero e o que o mapa
            // anterior deixava, e continua inalcancavel — renderavel sem malha valida nao entra
            // na TLAS, entao nenhum hit enderaca esta linha.
            g.TriangleSrv = g.MeshId != FGpuMeshTable::kMissingRow
                          ? _Meshes.GeometrySlotOf(R.Mesh) + 2u : 0u;
            std::memcpy(_Mapped + i * sizeof(FRTInstanceGeo), &g, sizeof(FRTInstanceGeo));
        }
    }

    void FRaytracingScene::RefreshInstanceGeo(const FScene& _Scene,
                                              const FGpuMaterialTable& _Materials,
                                              const FGpuMeshTable& _Meshes) {
        if (!InstanceGeoBuf || InstanceGeoCount == 0) return;
        const u32 Count = std::min(InstanceGeoCount,
                                   static_cast<u32>(_Scene.Renderables().size()));
        u8*         Mapped = nullptr;
        D3D12_RANGE NoRead{ 0, 0 };
        if (FAILED(InstanceGeoBuf->Map(0, &NoRead, reinterpret_cast<void**>(&Mapped)))) return;
        FillInstanceGeo(_Scene, _Materials, _Meshes, Mapped, Count);
        InstanceGeoBuf->Unmap(0, nullptr);
    }

    bool FRaytracingScene::RecordTlasRebuild(ID3D12GraphicsCommandList4* _CL, const FScene& _Scene,
                                             const Vec3& _TlasOrigin, u32 _FrameSlot) {
        if (!Built || !Tlas || !TlasScratch || !_CL) return false;

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
        CollectInstances(_Scene, _TlasOrigin, Instances);
        if (Instances.empty() || Instances.size() > InstanceCapacity_) return false;

        const u32 Slot = _FrameSlot % kInstanceSlots;
        std::memcpy(InstanceMapped[Slot], Instances.data(),
                    Instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TInputs{};
        TInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        TInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
        TInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        TInputs.NumDescs      = static_cast<UINT>(Instances.size());
        TInputs.InstanceDescs = InstanceUpload[Slot]->GetGPUVirtualAddress();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TBuild{};
        TBuild.Inputs                           = TInputs;
        TBuild.ScratchAccelerationStructureData = TlasScratch->GetGPUVirtualAddress();
        TBuild.DestAccelerationStructureData    = Tlas->GetGPUVirtualAddress();
        _CL->BuildRaytracingAccelerationStructure(&TBuild, 0, nullptr);
        // WAR/RAW: o proximo consumidor (trace) e o proximo rebuild (scratch) esperam o build.
        GlobalUAVBarrier(_CL);

        InstanceCount_ = static_cast<u32>(Instances.size());
        TlasOrigin_     = _TlasOrigin;
        return true;
    }

    void FRaytracingScene::Build(FD3D12Device& _Device, FCommandQueue& _Queue,
                                 FTextureSRVHeap& _SRVHeap, const FScene& _Scene,
                                 const FGpuMaterialTable& _Materials,
                                 const FGpuMeshTable& _Meshes, const Vec3& _TlasOrigin) {
        if (!_Device.RaytracingSupported() || !_Device.Device5())
            throw std::runtime_error("DXR Tier 1.2 obrigatorio para construir a cena OMM");

        Release(_SRVHeap);
        ID3D12Device5* Dev5 = _Device.Device5();

        _Queue.ResetForRecording();
        ComPtr<ID3D12GraphicsCommandList4> CL;
        if (FAILED(_Queue.List()->QueryInterface(IID_PPV_ARGS(&CL)))) {
            throw std::runtime_error(
                "ID3D12GraphicsCommandList4 indisponivel; nao existe caminho sem OMM");
        }

        struct FBlasKey {
            const FGpuMesh*             Mesh = nullptr;
            const FOpacityMicromapData* Omm  = nullptr;
        };
        std::vector<FBlasKey> UniqueBlas;
        for (const FRenderable& R : _Scene.Renderables()) {
            if (!R.Mesh || !R.Mesh->IsValid()) continue;

            const bool AlphaTest = R.Material && R.Material->Constants.AlphaTest;
            const FOpacityMicromapData* Omm = AlphaTest ? R.OpacityMicromap.get() : nullptr;
            if (AlphaTest && (!Omm || !Omm->Valid()))
                throw std::runtime_error("Material alpha-tested sem OMM valido; nao existe fallback DXR");
            // TODAS as quatro entradas do bake, e nao so alpha/cutoff: ver FOmmInputKey. Um
            // micro-map obsoleto nao produz excecao no DXR — produz recorte errado em folhagem.
            if (AlphaTest && !Omm->MatchesInputs(FOmmInputKey::From(R.Mesh, R.Material)))
                throw std::runtime_error(
                    "OMM desatualizado: uma entrada do bake (malha, textura de alpha, alpha ou "
                    "cutoff) mudou e o rebake nao alcancou. O RebakeStaleOpacityMicromaps roda no "
                    "topo do frame, antes disto — chegar aqui significa que ele falhou e "
                    "logou por que");

            auto& Variants = BlasByMesh[R.Mesh];
            if (Variants.find(Omm) != Variants.end()) continue;
            Variants.emplace(Omm, D3D12_GPU_VIRTUAL_ADDRESS{ 0 });
            UniqueBlas.push_back({ R.Mesh, Omm });
        }
        if (UniqueBlas.empty()) {
            LogDebug("[GI] - Cena sem geometria; AS nao construida");
            return;
        }
        const u32 NumBlas   = static_cast<u32>(UniqueBlas.size());

        // VB/IB dos meshes ja vivem em estado combinado de leitura que inclui NON_PIXEL
        // (GpuMesh.cpp) — o build de BLAS le direto, sem transicao de ida e volta.

        // Passo 1: prebuild de todos os BLAS p/ dimensionar UM scratch e UM pool de build
        // suballocados por offset (alinhamento de AS = 256B) em vez de um committed resource
        // (heap >= 64KB) por BLAS — best practice NVIDIA/UE (RHIBuildAccelerationStructures).
        struct FBlasPlan {
            D3D12_RAYTRACING_GEOMETRY_DESC Geo{};
            D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC Triangles{};
            D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC OmmLinkage{};
            D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC OmmArrayDesc{};
            GpuResources::FUploadBuffer OmmArrayInput;
            GpuResources::FUploadBuffer OmmDescInput;
            GpuResources::FUploadBuffer OmmIndexInput;
            ComPtr<ID3D12Resource> OmmArray;
            UINT64 OmmScratchOffset = 0;
            UINT64 ScratchOffset = 0;
            UINT64 BuildOffset   = 0;
            UINT64 CompactOffset = 0;
        };
        std::vector<FBlasPlan> Plans(NumBlas);
        UINT64 TotalScratch = 0, TotalBuild = 0;
        // VRAM dos OMM Arrays, que nao moram no BlasPool: o log dava so a CONTAGEM deles.
        UINT64 TotalOmmArrayBytes = 0;
        for (u32 i = 0; i < NumBlas; ++i) {
            const FBlasKey& Key = UniqueBlas[i];
            const FGpuMesh* M = Key.Mesh;
            FBlasPlan& P = Plans[i];
            P.Triangles.VertexBuffer.StartAddress  = M->VertexBufferGPUVA();
            P.Triangles.VertexBuffer.StrideInBytes = M->VertexStride();
            P.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
            P.Triangles.VertexCount                = M->VertexCount();
            P.Triangles.IndexBuffer                = M->IndexBufferGPUVA();
            P.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
            P.Triangles.IndexCount                 = M->GetIndexCount();
            P.Triangles.Transform3x4               = 0;

            if (Key.Omm) {
                const FOpacityMicromapData& Omm = *Key.Omm;
                P.OmmIndexInput = GpuResources::CreateUploadBuffer(
                    Dev5, static_cast<u64>(Omm.Indices.size()), 1, false);
                std::memcpy(P.OmmIndexInput.Mapped, Omm.Indices.data(), Omm.Indices.size());

                if (!Omm.Descs.empty()) {
                    P.OmmArrayInput = GpuResources::CreateUploadBuffer(
                        Dev5, static_cast<u64>(Omm.ArrayData.size()), 1, false);
                    P.OmmDescInput = GpuResources::CreateUploadBuffer(
                        Dev5, static_cast<u64>(Omm.Descs.size() * sizeof(Omm.Descs[0])), 1, false);
                    std::memcpy(P.OmmArrayInput.Mapped, Omm.ArrayData.data(), Omm.ArrayData.size());
                    std::memcpy(P.OmmDescInput.Mapped, Omm.Descs.data(),
                                Omm.Descs.size() * sizeof(Omm.Descs[0]));

                    P.OmmArrayDesc.NumOmmHistogramEntries = static_cast<UINT>(Omm.Histogram.size());
                    P.OmmArrayDesc.pOmmHistogram          = Omm.Histogram.data();
                    P.OmmArrayDesc.InputBuffer            = P.OmmArrayInput.Address(0);
                    P.OmmArrayDesc.PerOmmDescs = {
                        P.OmmDescInput.Address(0), sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC)
                    };

                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS OmmInputs{};
                    OmmInputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
                    OmmInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                    OmmInputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                    OmmInputs.NumDescs    = 1;
                    OmmInputs.pOpacityMicromapArrayDesc = &P.OmmArrayDesc;
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO OmmInfo{};
                    Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&OmmInputs, &OmmInfo);
                    if (OmmInfo.ResultDataMaxSizeInBytes == 0 || OmmInfo.ScratchDataSizeInBytes == 0)
                        throw std::runtime_error("Driver recusou o prebuild do OMM Array");
                    P.OmmScratchOffset = TotalScratch;
                    TotalScratch += AlignAS(OmmInfo.ScratchDataSizeInBytes);
                    P.OmmArray = CreateUAVBuffer(Dev5, OmmInfo.ResultDataMaxSizeInBytes,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                    OmmArrays.push_back(P.OmmArray);
                    TotalOmmArrayBytes += OmmInfo.ResultDataMaxSizeInBytes;
                }

                P.OmmLinkage.OpacityMicromapIndexBuffer = {
                    P.OmmIndexInput.Address(0), Omm.IndexStride
                };
                P.OmmLinkage.OpacityMicromapIndexFormat = Omm.IndexFormat;
                P.OmmLinkage.OpacityMicromapBaseLocation = 0;
                P.OmmLinkage.OpacityMicromapArray =
                    P.OmmArray ? P.OmmArray->GetGPUVirtualAddress() : 0;
                P.Geo.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
                P.Geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                P.Geo.OmmTriangles.pTriangles  = &P.Triangles;
                P.Geo.OmmTriangles.pOmmLinkage = &P.OmmLinkage;
            } else {
                P.Geo.Type      = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                P.Geo.Flags     = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                P.Geo.Triangles = P.Triangles;
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
            Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            Inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                                  | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            Inputs.NumDescs       = 1;
            Inputs.pGeometryDescs = &P.Geo;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info{};
            Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Info);
            P.ScratchOffset = TotalScratch;
            P.BuildOffset   = TotalBuild;
            TotalScratch += AlignAS(Info.ScratchDataSizeInBytes);
            TotalBuild   += AlignAS(Info.ResultDataMaxSizeInBytes);
        }

        ComPtr<ID3D12Resource> Scratch = CreateUAVBuffer(Dev5, TotalScratch,
            D3D12_RESOURCE_STATE_COMMON);
        ComPtr<ID3D12Resource> BuildPool = CreateUAVBuffer(Dev5, TotalBuild,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        // Tamanhos compactados emitidos pelo build (postbuild info) + readback p/ CPU.
        ComPtr<ID3D12Resource> PostbuildBuf = CreateUAVBuffer(Dev5,
            static_cast<UINT64>(NumBlas) * sizeof(UINT64), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> ReadbackBuf = GpuResources::CreateReadbackBuffer(
            Dev5, static_cast<u64>(NumBlas) * sizeof(UINT64));

        // Passo 2: os OMM Arrays precisam existir antes de qualquer BLAS que os referencia.
        bool HasOmmBuild = false;
        for (FBlasPlan& P : Plans) {
            if (!P.OmmArray) continue; // bake so de indices especiais nao possui array
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build{};
            Build.Inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
            Build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Build.Inputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            Build.Inputs.NumDescs    = 1;
            Build.Inputs.pOpacityMicromapArrayDesc = &P.OmmArrayDesc;
            Build.ScratchAccelerationStructureData = Scratch->GetGPUVirtualAddress() + P.OmmScratchOffset;
            Build.DestAccelerationStructureData    = P.OmmArray->GetGPUVirtualAddress();
            CL->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);
            HasOmmBuild = true;
        }
        if (HasOmmBuild) GlobalUAVBarrier(CL.Get());

        // Builds de BLAS no pool + emissao do tamanho compactado de cada um.
        for (u32 i = 0; i < NumBlas; ++i) {
            FBlasPlan& P = Plans[i];
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
            Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            Inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                                  | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            Inputs.NumDescs       = 1;
            Inputs.pGeometryDescs = &P.Geo;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build{};
            Build.Inputs                           = Inputs;
            Build.ScratchAccelerationStructureData = Scratch->GetGPUVirtualAddress() + P.ScratchOffset;
            Build.DestAccelerationStructureData    = BuildPool->GetGPUVirtualAddress() + P.BuildOffset;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC Post{};
            Post.InfoType   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
            Post.DestBuffer = PostbuildBuf->GetGPUVirtualAddress() + i * sizeof(UINT64);
            CL->BuildRaytracingAccelerationStructure(&Build, 1, &Post);
        }
        GlobalUAVBarrier(CL.Get());

        {
            std::vector<D3D12_RESOURCE_BARRIER> ToCopy;
            PushTransition(ToCopy, PostbuildBuf.Get(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            CL->ResourceBarrier(static_cast<UINT>(ToCopy.size()), ToCopy.data());
            CL->CopyResource(ReadbackBuf.Get(), PostbuildBuf.Get());
        }

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL.Get() };
        _Queue.ExecuteAndSync(Lists, 1);

        // Passo 3: readback dos tamanhos compactados e layout do pool final. Compaction de BLAS
        // estatico ~50% de VRAM sem custo de trace (NVIDIA); se algum tamanho vier 0 (readback
        // quebrado), cai pro pool de build sem compactar.
        UINT64 TotalCompact = 0;
        bool   Compact      = true;
        {
            void* Mapped = nullptr;
            D3D12_RANGE ReadRange{ 0, static_cast<SIZE_T>(NumBlas * sizeof(UINT64)) };
            SMILE_HR(ReadbackBuf->Map(0, &ReadRange, &Mapped));
            const UINT64* Sizes = static_cast<const UINT64*>(Mapped);
            for (u32 i = 0; i < NumBlas; ++i) {
                if (Sizes[i] == 0) { Compact = false; break; }
                Plans[i].CompactOffset = TotalCompact;
                TotalCompact += AlignAS(Sizes[i]);
            }
            D3D12_RANGE NoWrite{ 0, 0 };
            ReadbackBuf->Unmap(0, &NoWrite);
        }

        _Queue.ResetForRecording();

        if (Compact) {
            BlasPool = CreateUAVBuffer(Dev5, TotalCompact,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            for (u32 i = 0; i < NumBlas; ++i) {
                CL->CopyRaytracingAccelerationStructure(
                    BlasPool->GetGPUVirtualAddress() + Plans[i].CompactOffset,
                    BuildPool->GetGPUVirtualAddress() + Plans[i].BuildOffset,
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
            }
            GlobalUAVBarrier(CL.Get());
            for (u32 i = 0; i < NumBlas; ++i)
                BlasByMesh[UniqueBlas[i].Mesh][UniqueBlas[i].Omm] =
                    BlasPool->GetGPUVirtualAddress() + Plans[i].CompactOffset;
        } else {
            LogWarning("[GI] - Readback de compaction invalido; BLAS sem compactar");
            BlasPool = BuildPool;
            for (u32 i = 0; i < NumBlas; ++i)
                BlasByMesh[UniqueBlas[i].Mesh][UniqueBlas[i].Omm] =
                    BlasPool->GetGPUVirtualAddress() + Plans[i].BuildOffset;
        }
        BlasCount_ = NumBlas;

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
        CollectInstances(_Scene, _TlasOrigin, Instances);
        InstanceCount_ = static_cast<u32>(Instances.size());

        if (!Instances.empty()) {
            // Infra persistente (rebuild de TLAS por frame no editor): uploads versionados
            // por frame em voo + TLAS/scratch dimensionados p/ a capacidade maxima — TODOS
            // os renderables, pois Visible pode ligar depois do load.
            // Capacidade COM folga (SceneCapacityFor): sem ela, o primeiro objeto criado no
            // editor ja nao cabia e o RecordTlasRebuild recusava, forcando um Build inteiro.
            InstanceCapacity_ =
                SceneCapacityFor(static_cast<u32>(_Scene.Renderables().size()));
            const UINT64 UploadSize =
                static_cast<UINT64>(InstanceCapacity_) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
            for (u32 s = 0; s < kInstanceSlots; ++s) {
                const GpuResources::FUploadBuffer Upload =
                    GpuResources::CreateUploadBuffer(Dev5, UploadSize, 1, false);
                InstanceUpload[s] = Upload.Resource;
                InstanceMapped[s] = Upload.Mapped;
            }
            std::memcpy(InstanceMapped[0], Instances.data(),
                        Instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TInputs{};
            TInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            TInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
            TInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            TInputs.NumDescs      = InstanceCapacity_; // prebuild no pior caso (rebuilds reusam)
            TInputs.InstanceDescs = InstanceUpload[0]->GetGPUVirtualAddress();

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TInfo{};
            Dev5->GetRaytracingAccelerationStructurePrebuildInfo(&TInputs, &TInfo);
            TlasScratch = CreateUAVBuffer(Dev5, AlignAS(TInfo.ScratchDataSizeInBytes),
                                          D3D12_RESOURCE_STATE_COMMON);
            Tlas = CreateUAVBuffer(Dev5, AlignAS(TInfo.ResultDataMaxSizeInBytes),
                                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TBuild{};
            TBuild.Inputs                           = TInputs;
            TBuild.Inputs.NumDescs                  = static_cast<UINT>(Instances.size());
            TBuild.ScratchAccelerationStructureData = TlasScratch->GetGPUVirtualAddress();
            TBuild.DestAccelerationStructureData    = Tlas->GetGPUVirtualAddress();
            CL->BuildRaytracingAccelerationStructure(&TBuild, 0, nullptr);
        }

        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists2[] = { CL.Get() };
        _Queue.ExecuteAndSync(Lists2, 1);
        // Pool de build, scratch e buffers de postbuild/readback morrem aqui (GPU ja sincronizada).

        if (Instances.empty()) {
            LogDebug("[GI] - Nenhuma instancia visivel; TLAS nao construida");
            return;
        }

        TlasSRVSlot_ = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC SRV{};
        SRV.ViewDimension                           = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        SRV.Shader4ComponentMapping                 = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SRV.RaytracingAccelerationStructure.Location = Tlas->GetGPUVirtualAddress();
        _SRVHeap.CreateSRV(_Device.Native(), nullptr, SRV, TlasSRVSlot_);

        // === Snapshot InstanceGeo ==========================================================
        // Depois da TLAS, e nao antes, porque o snapshot so tem sentido acompanhado dela: quem o
        // le chega pelo InstanceID de um hit. Sem TLAS o Build ja retornou acima, e ai o
        // InstanceGeoSRV segue invalido — o mesmo criterio que o FDDGI aplicava quando era dono
        // disto (ele desistia com TlasSRVSlot == kInvalidSlot).
        //
        // Os descritores bindless de geometria — TRES por malha unica, [VB][IB][RTTri] — moravam
        // AQUI ate a Fase 2 e passaram para a FGpuMeshTable. Nao e arrumacao: a decisao D12 do
        // plano diz que o ray tracing e consumidor da cena e nao dono dela, e a enumeracao das
        // malhas unicas que acompanhava esses descritores ja tinha divergido uma vez quando o
        // FDDGI a reconstruia "com um criterio identico, palavra por palavra". Agora ela tem um
        // dono so, e este Build le a lista de la (ver o laco dos BLAS acima).

        // Com folga (SceneCapacityFor): objeto criado no editor cabe sem realocar descritor nem
        // refazer o Build. So os [0, NumRenderables) sao preenchidos — FillInstanceGeo le a lista
        // da cena, e ir alem dela seria leitura fora do vetor. A cauda vai a zero: nenhuma
        // instancia da TLAS aponta para la (a TLAS so tem as reais), mas zero e um estado legivel
        // se algum dia um indice errado chegar ate aqui.
        const u32 NumRenderables = static_cast<u32>(_Scene.Renderables().size());
        const u32 GeoCapacity    = SceneCapacityFor(NumRenderables);
        const GpuResources::FUploadBuffer GeoUpload = GpuResources::CreateUploadBuffer(
            Dev5, static_cast<UINT64>(GeoCapacity) * sizeof(FRTInstanceGeo), 1,
            /*ForConstantBuffer*/ false);
        InstanceGeoBuf   = GeoUpload.Resource;
        InstanceGeoCount = GeoCapacity;
        FillInstanceGeo(_Scene, _Materials, _Meshes, GeoUpload.Mapped, NumRenderables);
        std::memset(GeoUpload.Mapped + static_cast<size_t>(NumRenderables) * sizeof(FRTInstanceGeo),
                    0, static_cast<size_t>(GeoCapacity - NumRenderables) * sizeof(FRTInstanceGeo));
        InstanceGeoBuf->Unmap(0, nullptr);

        InstanceGeoSRVSlot_ = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC GeoBufSrv{};
        GeoBufSrv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        GeoBufSrv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GeoBufSrv.Format                     = DXGI_FORMAT_UNKNOWN;
        GeoBufSrv.Buffer.FirstElement        = 0;
        GeoBufSrv.Buffer.NumElements         = GeoCapacity; // = tamanho do buffer, nao da cena
        GeoBufSrv.Buffer.StructureByteStride = sizeof(FRTInstanceGeo);
        _SRVHeap.CreateSRV(_Device.Native(), InstanceGeoBuf.Get(), GeoBufSrv, InstanceGeoSRVSlot_);

        Built = true;
        TlasOrigin_ = _TlasOrigin;
        const double BuildMB   = static_cast<double>(TotalBuild)   / (1024.0 * 1024.0);
        const double FinalMB   = static_cast<double>(Compact ? TotalCompact : TotalBuild) / (1024.0 * 1024.0);
        const double OmmMB     = static_cast<double>(TotalOmmArrayBytes) / (1024.0 * 1024.0);
        LogDebug("[GI] - TLAS construida: " + std::to_string(InstanceCount_) +
                " instancias / " + std::to_string(NumBlas) + " BLAS (pool " +
                std::to_string(FinalMB).substr(0, 6) + " MB, build " +
                std::to_string(BuildMB).substr(0, 6) + " MB, compaction " +
                (Compact ? "ON" : "OFF") + ") / " + std::to_string(OmmArrays.size()) +
                " OMM Arrays (" + std::to_string(OmmMB).substr(0, 6) + " MB)");
    }
}
