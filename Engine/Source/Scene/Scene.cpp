#include "Smile/Scene/Scene.h"

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

    FRenderable& FScene::AddRenderable(const FRenderable& _Renderable) {
        RenderableList.push_back(_Renderable);
        return RenderableList.back();
    }

    void FScene::Clear() {
        RenderableList.clear();
        MeshLibrary.clear();
    }
}
