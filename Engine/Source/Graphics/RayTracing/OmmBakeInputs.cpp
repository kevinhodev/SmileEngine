#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"

#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h"

namespace Smile {
    void FOmmBakeInputStore::RetainMesh(const FGpuMesh* _Mesh, const FMesh& _Source) {
        if (!_Mesh || _Source.Vertices.empty() || _Source.Indices.empty()) return;
        // Dedup por malha: N pares (malha, material) mascarados compartilham a geometria, e o
        // micro-map difere entre eles pelo MATERIAL, nao pelos vertices.
        if (Meshes_.contains(_Mesh)) return;

        FRetainedMesh R;
        R.TexCoords.resize(_Source.Vertices.size() * 2);
        for (size_t V = 0; V < _Source.Vertices.size(); ++V) {
            R.TexCoords[V * 2 + 0] = _Source.Vertices[V].TexCoord[0];
            R.TexCoords[V * 2 + 1] = _Source.Vertices[V].TexCoord[1];
        }
        R.Indices = _Source.Indices;

        RetainedBytes_ += static_cast<u64>(R.TexCoords.size()) * sizeof(f32) +
                          static_cast<u64>(R.Indices.size()) * sizeof(u32);
        Meshes_.emplace(_Mesh, std::move(R));
    }

    void FOmmBakeInputStore::RetainTexturePath(const FTexture* _Texture, std::wstring _Path,
                                               bool _IsNormalMap, bool _sRGB) {
        if (!_Texture || _Path.empty()) return;
        // O ULTIMO a chegar vence: uma textura pode ser recriada no mesmo ponteiro? Nao hoje (o
        // ImportedTextures so cresce), mas se acontecer, a procedencia mais recente e a correta.
        Textures_[_Texture] = FTextureSource{ std::move(_Path), _IsNormalMap, _sRGB };
    }

    void FOmmBakeInputStore::Clear() {
        Meshes_.clear();
        Textures_.clear();
        RetainedBytes_ = 0;
    }

    FOmmGeometry FOmmBakeInputStore::GeometryOf(const FGpuMesh* _Mesh) const {
        const auto It = _Mesh ? Meshes_.find(_Mesh) : Meshes_.end();
        if (It == Meshes_.end()) return {};

        FOmmGeometry G;
        G.TexCoords      = It->second.TexCoords.data();
        // Compactadas: 2 floats por vertice, sem o resto do Vertex no meio.
        G.TexCoordStride = sizeof(f32) * 2;
        G.VertexCount    = static_cast<u32>(It->second.TexCoords.size() / 2);
        G.Indices        = It->second.Indices.data();
        G.IndexCount     = static_cast<u32>(It->second.Indices.size());
        return G;
    }

    const FOmmBakeInputStore::FTextureSource* FOmmBakeInputStore::TextureSourceOf(
        const FTexture* _Texture) const {
        const auto It = _Texture ? Textures_.find(_Texture) : Textures_.end();
        return It == Textures_.end() ? nullptr : &It->second;
    }
}
