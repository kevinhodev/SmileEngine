#include "Smile/Scene/Scene.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"

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

    std::vector<FGpuMesh*> FScene::AddMeshesBatch(ID3D12Device* _Device, FCommandQueue& _Queue,
                                                  const std::vector<FMesh>& _Meshes) {
        std::vector<FGpuMesh*> Out;
        Out.reserve(_Meshes.size());
        constexpr size_t kStagingBudget = 256ull * 1024 * 1024; // ~256 MB de staging por chunk

        size_t i = 0;
        while (i < _Meshes.size()) {
            _Queue.ResetForRecording();
            ID3D12GraphicsCommandList* CommandList = _Queue.List();
            std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> Staging;
            size_t BatchBytes = 0;

            for (; i < _Meshes.size(); ++i) {
                auto Gpu = std::make_unique<FGpuMesh>();
                Gpu->RecordUploadDefault(_Device, CommandList, _Meshes[i], Staging);
                FGpuMesh* Ptr = Gpu.get();
                MeshLibrary.push_back(std::move(Gpu));
                Out.push_back(Ptr);
                BatchBytes += _Meshes[i].Vertices.size() * sizeof(Vertex)
                            + _Meshes[i].Indices.size()  * sizeof(u32);
                if (BatchBytes >= kStagingBudget) { ++i; break; }
            }

            SMILE_HR(CommandList->Close());
            ID3D12CommandList* Lists[] = { CommandList };
            _Queue.ExecuteAndSync(Lists, 1); // staging deste chunk liberado ao sair do escopo
        }
        return Out;
    }

    FRenderable& FScene::AddRenderable(const FRenderable& _Renderable) {
        RenderableList.push_back(_Renderable);
        return RenderableList.back();
    }

    void FScene::Clear() {
        RenderableList.clear();
        MeshLibrary.clear();
    }
}
