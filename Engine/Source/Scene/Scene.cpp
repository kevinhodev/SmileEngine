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
        return RenderableList.back();
    }

    FLight& FScene::AddLight(const FLight& _Light) {
        LightList.push_back(_Light);
        return LightList.back();
    }

    void FScene::Clear() {
        RenderableList.clear();
        MeshLibrary.clear();
        LightList.clear();
    }
}
