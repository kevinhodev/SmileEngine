#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"

#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Scene/CookedFormat.h"
#include "Smile/Core/Logger.h"

#include <fstream>

namespace Smile {
    void FOmmBakeInputStore::RetainMeshSource(const FGpuMesh* _Mesh, std::wstring _MeshFilePath,
                                              u32 _MeshIndex) {
        if (!_Mesh || _MeshFilePath.empty()) return;
        Meshes_.insert_or_assign(_Mesh, FMeshSource{ std::move(_MeshFilePath), _MeshIndex });
    }

    void FOmmBakeInputStore::RetainTexturePath(const FTexture* _Texture, std::wstring _Path,
                                               bool _IsNormalMap, bool _sRGB) {
        if (!_Texture || _Path.empty()) return;
        Textures_.insert_or_assign(_Texture, FTextureSource{ std::move(_Path), _IsNormalMap, _sRGB });
    }

    void FOmmBakeInputStore::Clear() {
        Meshes_.clear();
        Textures_.clear();
    }

    u64 FOmmBakeInputStore::RetainedBytes() const {
        u64 Bytes = 0;
        for (const auto& [Mesh, Source] : Meshes_)
            Bytes += sizeof(FMeshSource) + Source.FilePath.size() * sizeof(wchar_t);
        for (const auto& [Texture, Source] : Textures_)
            Bytes += sizeof(FTextureSource) + Source.Path.size() * sizeof(wchar_t);
        return Bytes;
    }

    const FOmmBakeInputStore::FTextureSource* FOmmBakeInputStore::TextureSourceOf(
        const FTexture* _Texture) const {
        const auto It = _Texture ? Textures_.find(_Texture) : Textures_.end();
        return It == Textures_.end() ? nullptr : &It->second;
    }

    bool FOmmBakeInputStore::LoadGeometry(const FGpuMesh* _Mesh, FLoadedGeometry& _Out) const {
        _Out.TexCoords.clear();
        _Out.Indices.clear();

        const auto It = _Mesh ? Meshes_.find(_Mesh) : Meshes_.end();
        if (It == Meshes_.end()) return false;
        const FMeshSource& Source = It->second;

        std::ifstream File(Source.FilePath, std::ios::binary);
        if (!File) {
            LogError("[OMM] Rebake: nao consegui abrir o .smesh da malha");
            return false;
        }

        SMeshHeader Header{};
        File.read(reinterpret_cast<char*>(&Header), sizeof(Header));
        if (!File || Header.Magic != kSMeshMagic || Header.Version != kCookedVersion) {
            // O arquivo mudou debaixo da cena carregada (recook enquanto o editor esta aberto).
            // Ler assim mesmo daria UVs de outra malha, e o bake sairia plausivel e errado.
            LogError("[OMM] Rebake: o .smesh nao bate com o formato carregado; recarregue a cena");
            return false;
        }
        if (Source.MeshIndex >= Header.MeshCount) {
            LogError("[OMM] Rebake: indice de malha fora do .smesh");
            return false;
        }

        // Layout do arquivo, o mesmo que o SceneLoader assume: [header][entries][blob].
        const std::streamoff EntryOffset =
            static_cast<std::streamoff>(sizeof(SMeshHeader)) +
            static_cast<std::streamoff>(Source.MeshIndex) * sizeof(SMeshEntry);
        SMeshEntry Entry{};
        File.seekg(EntryOffset);
        File.read(reinterpret_cast<char*>(&Entry), sizeof(Entry));
        if (!File) {
            LogError("[OMM] Rebake: tabela de meshes truncada");
            return false;
        }
        if (Entry.VertexCount == 0 || Entry.IndexCount == 0 || (Entry.IndexCount % 3) != 0) {
            LogError("[OMM] Rebake: entrada de malha sem triangulos validos");
            return false;
        }

        const std::streamoff GeometryBase =
            static_cast<std::streamoff>(sizeof(SMeshHeader)) +
            static_cast<std::streamoff>(Header.MeshCount) * sizeof(SMeshEntry);

        // Os vertices sao lidos inteiros e as UVs extraidas: o bake so quer 8 dos 32 B, mas ler
        // salteado do disco seria uma seek por vertice.
        std::vector<Vertex> Vertices(Entry.VertexCount);
        File.seekg(GeometryBase + static_cast<std::streamoff>(Entry.VertexOffset));
        File.read(reinterpret_cast<char*>(Vertices.data()),
                  static_cast<std::streamsize>(Entry.VertexCount) * sizeof(Vertex));
        if (!File) {
            LogError("[OMM] Rebake: blob de vertices truncado");
            return false;
        }

        _Out.Indices.resize(Entry.IndexCount);
        File.seekg(GeometryBase + static_cast<std::streamoff>(Entry.IndexOffset));
        File.read(reinterpret_cast<char*>(_Out.Indices.data()),
                  static_cast<std::streamsize>(Entry.IndexCount) * sizeof(u32));
        if (!File) {
            LogError("[OMM] Rebake: blob de indices truncado");
            _Out.Indices.clear();
            return false;
        }

        // Indice fora do vertex buffer produziria leitura fora do array dentro do baker.
        for (u32 I : _Out.Indices) {
            if (I < Entry.VertexCount) continue;
            LogError("[OMM] Rebake: indice fora do vertex buffer da malha");
            _Out.Indices.clear();
            return false;
        }

        _Out.TexCoords.resize(static_cast<size_t>(Entry.VertexCount) * 2);
        for (u32 V = 0; V < Entry.VertexCount; ++V) {
            _Out.TexCoords[V * 2 + 0] = Vertices[V].TexCoord[0];
            _Out.TexCoords[V * 2 + 1] = Vertices[V].TexCoord[1];
        }
        return true;
    }
}
