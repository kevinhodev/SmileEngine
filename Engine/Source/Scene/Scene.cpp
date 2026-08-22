#include "Smile/Scene/Scene.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/UploadQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>

namespace Smile {
    Mat44 FTransform::Matrix() const {
        const Mat44 S = Mat44::Scale(Scale);
        const Mat44 R = Mat44::RotationEulerXYZ(RotationEuler);
        const Mat44 T = Mat44::Translation(Position);
        return S * R * T;
    }

    void FRenderable::RefreshWorldBounds() {
        const Mat44 Model = Transform.Matrix();
        // Ponto x matriz a mao: o Mat44::operator*(Vec4) e da convencao COLUNA e o FTransform
        // monta LINHA (translacao em M[3], igual ao mul(float4(pos,1), MVP) dos shaders).
        auto ToWorld = [&Model](const Vec3& P) {
            return Vec3{
                P.X*Model.M[0][0] + P.Y*Model.M[1][0] + P.Z*Model.M[2][0] + Model.M[3][0],
                P.X*Model.M[0][1] + P.Y*Model.M[1][1] + P.Z*Model.M[2][1] + Model.M[3][1],
                P.X*Model.M[0][2] + P.Y*Model.M[1][2] + P.Z*Model.M[2][2] + Model.M[3][2] };
        };
        // Os 8 CANTOS, nao os dois extremos: sob rotacao a caixa alinhada aos eixos do resultado
        // nao e a imagem de min/max — projetar so os dois daria um volume menor que o objeto e o
        // culling comeria pedaco de geometria.
        Vec3 Min{  1e30f,  1e30f,  1e30f };
        Vec3 Max{ -1e30f, -1e30f, -1e30f };
        for (int Corner = 0; Corner < 8; ++Corner) {
            const Vec3 W = ToWorld(Vec3{ (Corner & 1) ? LocalAABBMax.X : LocalAABBMin.X,
                                         (Corner & 2) ? LocalAABBMax.Y : LocalAABBMin.Y,
                                         (Corner & 4) ? LocalAABBMax.Z : LocalAABBMin.Z });
            Min.X = std::min(Min.X, W.X); Max.X = std::max(Max.X, W.X);
            Min.Y = std::min(Min.Y, W.Y); Max.Y = std::max(Max.Y, W.Y);
            Min.Z = std::min(Min.Z, W.Z); Max.Z = std::max(Max.Z, W.Z);
        }
        AABBMin = Min;
        AABBMax = Max;
    }

    FGpuMesh* FScene::AddMesh(ID3D12Device* _Device, const FMesh& _Mesh) {
        auto Gpu = std::make_unique<FGpuMesh>();
        Gpu->Upload(_Device, _Mesh);
        FGpuMesh* Ptr = Gpu.get();
        MeshLibrary.push_back(std::move(Gpu));
        return Ptr;
    }

    std::vector<FGpuMesh*> FScene::AddMeshesBatch(ID3D12Device* _Device, FUploadQueue& _UploadQueue,
                                                  const std::vector<FMesh>& _Meshes) {
        // Pool de geometria: cada chunk vira UM buffer default-heap ([VBs desde 0][IBs])
        // + UM staging, com uma copia e uma barrier — em vez de 2 committed resources
        // (heap >=64KB cada) + 2 stagings por mesh. Os meshes viram fatias (InitFromPool);
        // o pool vive pelo refcount dos ComPtrs de cada mesh.
        std::vector<FGpuMesh*> Out;
        Out.reserve(_Meshes.size());
        constexpr u64 kChunkBudget = 256ull * 1024 * 1024;

        // Payload de RT por mesh. Da cena cozida ele ja vem no FMesh; do proxy do terreno (e de
        // qualquer malha construida em runtime) nao vem, e o scratch abaixo o gera UMA vez —
        // as duas passadas (dimensionar o chunk, copiar para o staging) leem o mesmo resultado.
        std::vector<std::vector<FRTTriangle>> RtScratch(_Meshes.size());
        auto RtOf = [&](size_t _M) -> const std::vector<FRTTriangle>& {
            return ResolveRTTriangles(_Meshes[_M], RtScratch[_M]);
        };

        size_t i = 0;
        while (i < _Meshes.size()) {
            // Layout do chunk: [VB][IB][RTTri]. Slices empacotados sem padding dentro de cada
            // regiao — cada tamanho e multiplo do stride da regiao, entao todo offset sai
            // multiplo dele (FirstElement dos SRVs bindless e exato). A regiao de IB comeca em
            // VbTotal (multiplo de 4, pois sizeof(Vertex)=32) e a de RT em VbTotal+IbTotal.
            //
            // ⚠️ O inicio da regiao de RT tem de ser multiplo de sizeof(FRTTriangle)=32. VbTotal
            // ja e multiplo de 32; IbTotal e multiplo de 4 e NAO de 32, entao a soma e alinhada
            // explicitamente. Sem isso o FirstElement truncaria e o mesh leria triangulos de
            // outro — desalinhamento silencioso, que so apareceria como facing errado.
            const size_t First = i;
            u64 VbTotal = 0, IbTotal = 0, RtTotal = 0;
            for (; i < _Meshes.size(); ++i) {
                const u64 RtBytes = RtOf(i).size() * sizeof(FRTTriangle);
                const u64 Add = _Meshes[i].Vertices.size() * sizeof(Vertex)
                              + _Meshes[i].Indices.size()  * sizeof(u32) + RtBytes;
                if (i > First && VbTotal + IbTotal + RtTotal + Add > kChunkBudget) break;
                VbTotal += _Meshes[i].Vertices.size() * sizeof(Vertex);
                IbTotal += _Meshes[i].Indices.size()  * sizeof(u32);
                RtTotal += RtBytes;
            }
            const u64 RtBase = ((VbTotal + IbTotal + sizeof(FRTTriangle) - 1)
                                / sizeof(FRTTriangle)) * sizeof(FRTTriangle);
            const u64 Total  = RtBase + RtTotal;
            if (Total == 0) {
                for (size_t m = First; m < i; ++m) {
                    auto Gpu = std::make_unique<FGpuMesh>(); // invalido (IndexCount 0)
                    Out.push_back(Gpu.get());
                    MeshLibrary.push_back(std::move(Gpu));
                }
                continue;
            }

            Microsoft::WRL::ComPtr<ID3D12Resource> Pool =
                FGpuMesh::CreatePoolBuffer(_Device, Total);

            // Staging do chunk INTEIRO (ate 256 MB). Fica fora do ring da fila de upload de
            // proposito: e uma alocacao grande e unica por chunk, nao o churn de uma por
            // recurso que o ring existe para resolver.
            const GpuResources::FUploadBuffer StagingBuffer =
                GpuResources::CreateUploadBuffer(_Device, Total, 1, false);
            Microsoft::WRL::ComPtr<ID3D12Resource> Staging = StagingBuffer.Resource;
            u8* Mapped = StagingBuffer.Mapped;

            u64 VbCursor = 0, IbCursor = VbTotal, RtCursor = RtBase;
            for (size_t m = First; m < i; ++m) {
                const FMesh& Mesh = _Meshes[m];
                const std::vector<FRTTriangle>& Rt = RtOf(m);
                const u64 VbSize = Mesh.Vertices.size() * sizeof(Vertex);
                const u64 IbSize = Mesh.Indices.size()  * sizeof(u32);
                const u64 RtSize = Rt.size()            * sizeof(FRTTriangle);
                auto Gpu = std::make_unique<FGpuMesh>();
                if (VbSize > 0 && IbSize > 0) {
                    std::memcpy(Mapped + VbCursor, Mesh.Vertices.data(), VbSize);
                    std::memcpy(Mapped + IbCursor, Mesh.Indices.data(),  IbSize);
                    if (RtSize > 0) std::memcpy(Mapped + RtCursor, Rt.data(), RtSize);
                    Gpu->InitFromPool(Pool, VbCursor, IbCursor, RtCursor,
                                      static_cast<u32>(Mesh.Vertices.size()),
                                      static_cast<u32>(Mesh.Indices.size()),
                                      static_cast<u32>(Rt.size()));
                    VbCursor += VbSize;
                    IbCursor += IbSize;
                    RtCursor += RtSize;
                }
                Out.push_back(Gpu.get());
                MeshLibrary.push_back(std::move(Gpu));
            }
            // Sem Unmap: o staging da fabrica e mapeado de forma persistente e morre com o
            // Keep abaixo, quando a fence da copia passar.

            // Fila COPY, sem bloquear e sem barrier: buffer promove/decai implicitamente
            // (VB/IB/SRV de BLAS leem via promotion na fila direta). O staging fica retido
            // pela fila; o SceneLoader da o WaitIdle antes do primeiro consumo.
            ID3D12GraphicsCommandList* CommandList = _UploadQueue.Begin();
            CommandList->CopyBufferRegion(Pool.Get(), 0, Staging.Get(), 0, Total);
            std::vector<ComPtr<ID3D12Resource>> Keep;
            Keep.push_back(std::move(Staging));
            _UploadQueue.Submit(std::move(Keep));
        }
        return Out;
    }

    FRenderable& FScene::AddRenderable(const FRenderable& _Renderable) {
        RenderableList.push_back(_Renderable);
        FRenderable& Added = RenderableList.back();
        Added.Id = AllocObjectId(); // identidade nova mesmo se veio de uma copia
        RenderableIndexById_.emplace(Added.Id,
                                     static_cast<u32>(RenderableList.size() - 1));
        ++StructureVersion_;
        // Objeto que nasce ou morre muda o CONTEUDO do mapa estatico, nao so o indice: o
        // shadow map cacheado precisa ser re-rasterizado. Ver StaticCastersVersion.
        ++StaticCastersVersion_;
        // Tambem a de transforms: a TLAS ganhou uma instancia, e quem reage a ela e o rebuild
        // por frame do RenderFrame. Durante o load isto e so um contador subindo 2,5k vezes
        // antes de existir TLAS — o custo e zero e a alternativa (bumpar so em quem cria com a
        // cena viva) e a classe de bug que faz o objeto novo nao aparecer no GI.
        ++TransformsVersion_;
        return Added;
    }

    bool FScene::RemoveRenderable(u64 _Id) {
        const int Index = IndexOfRenderable(_Id);
        if (Index < 0) return false;
        // Erase ESTAVEL, nao swap-and-pop: as pastas do Scene Outliner sao ranges [begin,end)
        // sobre esta lista, e trocar o removido com o ultimo jogaria uma mesh de outro asset
        // para dentro de uma pasta alheia. O memmove de ~2,5k structs e irrelevante numa acao
        // de editor.
        RenderableList.erase(RenderableList.begin() + Index);
        RebuildRenderableIndex();
        ++StructureVersion_;
        // Objeto que nasce ou morre muda o CONTEUDO do mapa estatico, nao so o indice: o
        // shadow map cacheado precisa ser re-rasterizado. Ver StaticCastersVersion.
        ++StaticCastersVersion_;
        ++TransformsVersion_; // a TLAS tem uma instancia a menos
        return true;
    }

    FRenderable* FScene::DuplicateRenderable(u64 _Id) {
        const int Index = IndexOfRenderable(_Id);
        if (Index < 0) return nullptr;
        // Copia por VALOR antes do push_back: o push_back pode realocar o vetor e uma referencia
        // para a fonte viraria ponteiro solto no meio da propria copia.
        FRenderable Copy = RenderableList[Index];
        Copy.Name += " (copia)";
        // Mantem o CookedIndex da fonte (e por onde a persistencia sabe que geometria recriar) e
        // marca como criada no editor, para nao ser confundida com o objeto original do asset.
        Copy.Spawned = true;
        // O FGpuMesh e compartilhado de proposito: o BLAS e cacheado por ponteiro de mesh
        // (FRaytracingScene::BlasByMesh), entao a copia entra na TLAS sem construir BLAS novo
        // nem gastar VRAM de geometria.
        return &AddRenderable(Copy); // Id novo + as duas versoes, como qualquer criacao
    }

    void FScene::RebuildRenderableIndex() {
        RenderableIndexById_.clear();
        RenderableIndexById_.reserve(RenderableList.size());
        for (u32 i = 0; i < static_cast<u32>(RenderableList.size()); ++i)
            RenderableIndexById_.emplace(RenderableList[i].Id, i);
    }

    int FScene::IndexOfRenderable(u64 _Id) const {
        if (_Id == 0) return -1;
        const auto It = RenderableIndexById_.find(_Id);
        return It == RenderableIndexById_.end() ? -1 : static_cast<int>(It->second);
    }

    FRenderable* FScene::FindRenderable(u64 _Id) {
        const int Index = IndexOfRenderable(_Id);
        return Index < 0 ? nullptr : &RenderableList[Index];
    }

    const FRenderable* FScene::FindRenderable(u64 _Id) const {
        const int Index = IndexOfRenderable(_Id);
        return Index < 0 ? nullptr : &RenderableList[Index];
    }

    u64 FScene::IdAt(u32 _Index) const {
        return _Index < RenderableList.size() ? RenderableList[_Index].Id : 0ull;
    }

    FSceneObjectRef FScene::FindObject(u64 _Id) const {
        if (_Id == 0) return {};
        if (const auto It = RenderableIndexById_.find(_Id); It != RenderableIndexById_.end())
            return { _Id, ESceneObject::Renderable, It->second };
        // Luz por varredura linear, e nao por mapa: sao dezenas, contra milhares de renderaveis,
        // e — o que decide — o editor muta LightList com push_back/erase diretos (LightsBridge),
        // entao um mapa aqui ficaria podre sem que nada avisasse. A varredura nao tem estado para
        // apodrecer. Nada no caminho de frame chama isto.
        for (u32 i = 0; i < static_cast<u32>(LightList.size()); ++i)
            if (LightList[i].Id == _Id) return { _Id, ESceneObject::Light, i };
        return {};
    }

    FLight& FScene::AddLight(const FLight& _Light) {
        LightList.push_back(_Light);
        LightList.back().Id = AllocObjectId(); // identidade nova mesmo se veio de uma copia // identidade nova mesmo se veio de uma copia
        return LightList.back();
    }

    void FScene::Clear() {
        RenderableList.clear();
        RenderableIndexById_.clear();
        MeshLibrary.clear();
        LightList.clear();
        ++StructureVersion_;
        // Objeto que nasce ou morre muda o CONTEUDO do mapa estatico, nao so o indice: o
        // shadow map cacheado precisa ser re-rasterizado. Ver StaticCastersVersion.
        ++StaticCastersVersion_;
        // NextObjectId_ NAO volta a zero: um Id nunca pode ser reusado dentro
        // da sessao, senao uma referencia velha (selecao, undo futuro) passaria a apontar em
        // silencio para um objeto diferente da cena nova em vez de simplesmente nao resolver.
    }
}
