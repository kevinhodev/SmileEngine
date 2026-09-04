#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"

#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Scene/CookedFormat.h"
#include "Smile/Core/Logger.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace Smile {
    FOmmFileStamp FOmmFileStamp::Of(const std::wstring& _Path) {
        FOmmFileStamp S;
        std::error_code Ec;
        const std::filesystem::path P(_Path);
        const auto Size = std::filesystem::file_size(P, Ec);
        if (Ec) return S;
        const auto Time = std::filesystem::last_write_time(P, Ec);
        if (Ec) return S;
        S.SizeBytes = static_cast<u64>(Size);
        S.WriteTime = static_cast<i64>(Time.time_since_epoch().count());
        return S;
    }

    void FOmmBakeInputStore::RetainMeshSource(const FGpuMesh* _Mesh,
                                              const std::wstring& _MeshFilePath, u32 _MeshIndex,
                                              const FOmmFileStamp& _Stamp) {
        if (!_Mesh || _MeshFilePath.empty()) return;
        const auto [It, Inserted] = Meshes_.insert_or_assign(
            _Mesh, FMeshSource{ _MeshFilePath, _MeshIndex, _Stamp });
        if (Inserted)
            RetainedBytes_ += sizeof(FMeshSource) + _MeshFilePath.size() * sizeof(wchar_t);
    }

    void FOmmBakeInputStore::RetainTexturePath(const FTexture* _Texture, std::wstring _Path,
                                               bool _IsNormalMap, bool _sRGB) {
        if (!_Texture || _Path.empty()) return;
        const u64 Bytes = sizeof(FTextureSource) + _Path.size() * sizeof(wchar_t);
        // O carimbo sai daqui, e nao do chamador: e o unico ponto que ve o caminho no momento em
        // que a textura residente acabou de ser criada a partir dele.
        const FOmmFileStamp Stamp = FOmmFileStamp::Of(_Path);
        const auto [It, Inserted] = Textures_.insert_or_assign(
            _Texture, FTextureSource{ std::move(_Path), _IsNormalMap, _sRGB, Stamp });
        if (Inserted) RetainedBytes_ += Bytes;
    }

    void FOmmBakeInputStore::Clear() {
        Meshes_.clear();
        Textures_.clear();
        RetainedBytes_ = 0;
    }

    bool FOmmBakeInputStore::HasTextureSource(const FTexture* _Texture) const {
        return _Texture && Textures_.contains(_Texture);
    }

    bool FOmmBakeInputStore::LoadAlphaTexture(const FTexture* _Texture,
                                              FTextureCPUData& _Out) const {
        const auto It = _Texture ? Textures_.find(_Texture) : Textures_.end();
        if (It == Textures_.end()) return false;
        const FTextureSource& Source = It->second;

        if (FOmmFileStamp::Of(Source.Path) != Source.Stamp) {
            LogError("[OMM] Rebake: a textura de alpha mudou no disco desde o load; os pixels "
                     "relidos nao sao os que estao na GPU. Recarregue a cena");
            return false;
        }
        try {
            _Out = FTexture::LoadCPUAuto(Source.Path, Source.IsNormalMap, Source.sRGB);
        } catch (const std::exception& Error) {
            LogError(std::string("[OMM] Rebake: falha ao reler a textura de alpha: ") +
                     Error.what());
            return false;
        }
        return _Out.Valid();
    }

    bool FOmmBakeInputStore::LoadGeometry(const FGpuMesh* _Mesh, FLoadedGeometry& _Out) const {
        _Out.TexCoords.clear();
        _Out.Indices.clear();

        const auto It = _Mesh ? Meshes_.find(_Mesh) : Meshes_.end();
        if (It == Meshes_.end()) return false;
        const FMeshSource& Source = It->second;

        // O CARIMBO antes de qualquer leitura. Magic e versao nao bastam: um recook normal
        // preserva os dois e pode trocar UVs, indices ou a ordem das malhas — e ai o micro-map
        // novo descreveria geometria que nao esta nos buffers de GPU, sem nenhum sinal.
        if (FOmmFileStamp::Of(Source.FilePath) != Source.Stamp) {
            LogError("[OMM] Rebake: o .smesh mudou no disco desde o load (recook?); a geometria "
                     "lida nao seria a que esta na GPU. Recarregue a cena");
            return false;
        }

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
