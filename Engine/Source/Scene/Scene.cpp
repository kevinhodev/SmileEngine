#include "Smile/Scene/Scene.h"
#include "Smile/Graphics/UploadQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>

namespace Smile {
    Mat44 FTransform::Matrix() const {
        const Mat44 S = Mat44::Scale(Scale);
        const Mat44 R = Mat44::RotationX(RotationEuler.X)
                      * Mat44::RotationY(RotationEuler.Y)
                      * Mat44::RotationZ(RotationEuler.Z);
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

        size_t i = 0;
        while (i < _Meshes.size()) {
            // Layout do chunk. Slices de VB empacotados sem padding: cada tamanho e multiplo
            // de sizeof(Vertex), entao todo offset sai multiplo do stride (FirstElement dos
            // SRVs bindless e exato). A regiao de IB comeca em VbTotal (multiplo de 4).
            const size_t First = i;
            u64 VbTotal = 0, IbTotal = 0;
            for (; i < _Meshes.size(); ++i) {
                const u64 Add = _Meshes[i].Vertices.size() * sizeof(Vertex)
                              + _Meshes[i].Indices.size()  * sizeof(u32);
                if (i > First && VbTotal + IbTotal + Add > kChunkBudget) break;
                VbTotal += _Meshes[i].Vertices.size() * sizeof(Vertex);
                IbTotal += _Meshes[i].Indices.size()  * sizeof(u32);
            }
            const u64 Total = VbTotal + IbTotal;
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

            Microsoft::WRL::ComPtr<ID3D12Resource> Staging;
            u8* Mapped = nullptr;
            {
                D3D12_HEAP_PROPERTIES HeapProps{};
                HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC ResourceDesc{};
                ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
                ResourceDesc.Width            = Total;
                ResourceDesc.Height           = 1;
                ResourceDesc.DepthOrArraySize = 1;
                ResourceDesc.MipLevels        = 1;
                ResourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
                ResourceDesc.SampleDesc       = { 1, 0 };
                ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                         &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                         IID_PPV_ARGS(&Staging)));
                D3D12_RANGE NoReadRange{ 0, 0 };
                SMILE_HR(Staging->Map(0, &NoReadRange, reinterpret_cast<void**>(&Mapped)));
            }

            u64 VbCursor = 0, IbCursor = VbTotal;
            for (size_t m = First; m < i; ++m) {
                const FMesh& Mesh = _Meshes[m];
                const u64 VbSize = Mesh.Vertices.size() * sizeof(Vertex);
                const u64 IbSize = Mesh.Indices.size()  * sizeof(u32);
                auto Gpu = std::make_unique<FGpuMesh>();
                if (VbSize > 0 && IbSize > 0) {
                    std::memcpy(Mapped + VbCursor, Mesh.Vertices.data(), VbSize);
                    std::memcpy(Mapped + IbCursor, Mesh.Indices.data(),  IbSize);
                    Gpu->InitFromPool(Pool, VbCursor, IbCursor,
                                      static_cast<u32>(Mesh.Vertices.size()),
                                      static_cast<u32>(Mesh.Indices.size()));
                    VbCursor += VbSize;
                    IbCursor += IbSize;
                }
                Out.push_back(Gpu.get());
                MeshLibrary.push_back(std::move(Gpu));
            }
            Staging->Unmap(0, nullptr);

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
        // NextObjectId_ NAO volta a zero: um Id nunca pode ser reusado dentro
        // da sessao, senao uma referencia velha (selecao, undo futuro) passaria a apontar em
        // silencio para um objeto diferente da cena nova em vez de simplesmente nao resolver.
    }
}
