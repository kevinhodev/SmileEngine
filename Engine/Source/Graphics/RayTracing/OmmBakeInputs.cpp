#include "Smile/Graphics/RayTracing/OmmBakeInputs.h"

#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Scene/CookedFormat.h"
#include "Smile/Core/Logger.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

namespace Smile {
    namespace {
        // O sRGB do load nao sobrevive no FTextureCPUData como flag; ele vira a VARIANTE _SRGB do
        // formato (ver FTexture::LoadCPU e o ToSRGB do LoadDDSCPU). Recuperar dai e exato para o
        // que importa: reler com esta resposta produz o mesmo DXGI_FORMAT, e portanto o mesmo
        // fingerprint. Formatos sem variante sRGB (BC4/BC5) devolvem false e continuam batendo,
        // porque o ToSRGB tambem os deixa como estao.
        bool FormatIsSrgb(DXGI_FORMAT _Fmt) {
            switch (_Fmt) {
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_BC1_UNORM_SRGB:
                case DXGI_FORMAT_BC2_UNORM_SRGB:
                case DXGI_FORMAT_BC3_UNORM_SRGB:
                case DXGI_FORMAT_BC7_UNORM_SRGB:
                    return true;
                default:
                    return false;
            }
        }
    }

    FOmmContentHash OmmHashTexture(const FTextureCPUData& _Data) {
        if (!_Data.Valid()) return kOmmNoContentHash;
        const FMipData& Mip = _Data.Mips.front();
        if (Mip.Pixels.empty()) return kOmmNoContentHash;

        FOmmContentHash H = kOmmHashSeed;
        // Dimensoes, formato e IsNormalMap entram junto com os pixels: os tres mudam o alpha que o
        // baker extrai destes mesmos bytes.
        const u32 Meta[4] = { Mip.Width, Mip.Height, static_cast<u32>(_Data.Format),
                              _Data.IsNormalMap ? 1u : 0u };
        H = OmmHashAppend(H, Meta, sizeof(Meta));
        H = OmmHashAppend(H, Mip.Pixels.data(), Mip.Pixels.size());
        return OmmHashFinish(H);
    }

    void FOmmBakeInputStore::RetainMeshSource(const FGpuMesh* _Mesh,
                                              const std::wstring& _MeshFilePath, u32 _MeshIndex,
                                              FOmmContentHash _ContentHash) {
        // Fail-closed na ENTRADA: sem fingerprint nao ha o que validar depois, e registrar assim
        // criaria uma procedencia que aceita qualquer coisa.
        if (!_Mesh || _MeshFilePath.empty() || _ContentHash == kOmmNoContentHash) return;
        const auto [It, Inserted] = Meshes_.insert_or_assign(
            _Mesh, FMeshSource{ _MeshFilePath, _MeshIndex, _ContentHash });
        if (Inserted)
            RetainedBytes_ += sizeof(FMeshSource) + _MeshFilePath.size() * sizeof(wchar_t);
    }

    void FOmmBakeInputStore::RetainTexturePath(const FTexture* _Texture, std::wstring _Path,
                                               const FTextureCPUData& _Data) {
        const FOmmContentHash ContentHash = OmmHashTexture(_Data);
        if (!_Texture || _Path.empty() || ContentHash == kOmmNoContentHash) return;
        const u64 Bytes = sizeof(FTextureSource) + _Path.size() * sizeof(wchar_t);
        const auto [It, Inserted] = Textures_.insert_or_assign(
            _Texture, FTextureSource{ std::move(_Path), _Data.IsNormalMap,
                                      FormatIsSrgb(_Data.Format), ContentHash });
        if (Inserted) RetainedBytes_ += Bytes;
    }

    void FOmmBakeInputStore::ForgetTexture(const FTexture* _Texture) {
        const auto It = _Texture ? Textures_.find(_Texture) : Textures_.end();
        if (It == Textures_.end()) return;
        const u64 Bytes = sizeof(FTextureSource) + It->second.Path.size() * sizeof(wchar_t);
        RetainedBytes_ = RetainedBytes_ >= Bytes ? RetainedBytes_ - Bytes : 0;
        Textures_.erase(It);
    }

    void FOmmBakeInputStore::Clear() {
        Meshes_.clear();
        Textures_.clear();
        RetainedBytes_ = 0;
    }

    bool FOmmBakeInputStore::HasTextureSource(const FTexture* _Texture) const {
        return _Texture && Textures_.contains(_Texture);
    }

    bool FOmmBakeInputStore::MeshSourceOf(const FGpuMesh* _Mesh, std::wstring& _FilePath,
                                          u32& _MeshIndex, FOmmContentHash& _ContentHash) const {
        const auto It = _Mesh ? Meshes_.find(_Mesh) : Meshes_.end();
        if (It == Meshes_.end()) return false;
        _FilePath    = It->second.FilePath;
        _MeshIndex   = It->second.MeshIndex;
        _ContentHash = It->second.ContentHash;
        return true;
    }

    FOmmContentHash FOmmBakeInputStore::TextureHashOf(const FTexture* _Texture) const {
        const auto It = _Texture ? Textures_.find(_Texture) : Textures_.end();
        return It == Textures_.end() ? kOmmNoContentHash : It->second.ContentHash;
    }

    bool FOmmBakeInputStore::LoadAlphaTexture(const FTexture* _Texture,
                                              FTextureCPUData& _Out) const {
        const auto It = _Texture ? Textures_.find(_Texture) : Textures_.end();
        if (It == Textures_.end()) return false;
        const FTextureSource& Source = It->second;

        try {
            _Out = FTexture::LoadCPUAuto(Source.Path, Source.IsNormalMap, Source.sRGB);
        } catch (const std::exception& Error) {
            LogError(std::string("[OMM] Rebake: falha ao reler a textura de alpha: ") +
                     Error.what());
            return false;
        }
        if (!_Out.Valid()) return false;

        // O fingerprint depois da decodificacao, sobre os MESMOS bytes que o import hasheou.
        if (OmmHashTexture(_Out) != Source.ContentHash) {
            LogError("[OMM] Rebake: a textura de alpha no disco nao e a que esta na GPU (o arquivo "
                     "mudou desde o load). Recarregue a cena");
            _Out = {};
            return false;
        }
        return true;
    }

    bool FOmmBakeInputStore::LoadGeometry(const FGpuMesh* _Mesh, FLoadedGeometry& _Out) const {
        _Out.TexCoords.clear();
        _Out.Indices.clear();

        const auto It = _Mesh ? Meshes_.find(_Mesh) : Meshes_.end();
        if (It == Meshes_.end()) return false;
        const FMeshSource& Source = It->second;

        std::ifstream File(Source.FilePath, std::ios::binary | std::ios::ate);
        if (!File) {
            LogError("[OMM] Rebake: nao consegui abrir o .smesh da malha");
            return false;
        }
        const std::streamoff FileBytes = File.tellg();
        File.seekg(0);

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
        if (GeometryBase > FileBytes) {
            LogError("[OMM] Rebake: tabela de meshes maior que o arquivo");
            return false;
        }
        const u64 GeometryBytes = static_cast<u64>(FileBytes - GeometryBase);

        // OS RANGES ANTES DAS ALOCACOES. VertexCount e IndexCount vem do arquivo e dimensionam os
        // vetores abaixo: conferir depois de alocar significaria um arquivo corrompido pedindo
        // gigabytes antes de qualquer validacao. Mesmo criterio do ArrayFits do SceneLoader, e com
        // a multiplicacao checada contra overflow em vez de assumida.
        auto RangeFits = [GeometryBytes](u64 _Offset, u64 _Count, u64 _Stride) {
            if (_Count == 0) return true;
            if (_Count > (std::numeric_limits<u64>::max)() / _Stride) return false;
            const u64 Bytes = _Count * _Stride;
            return _Offset <= GeometryBytes && Bytes <= GeometryBytes - _Offset;
        };
        if (!RangeFits(Entry.VertexOffset, Entry.VertexCount, sizeof(Vertex)) ||
            !RangeFits(Entry.IndexOffset, Entry.IndexCount, sizeof(u32))) {
            LogError("[OMM] Rebake: blob de geometria truncado ou entrada fora do arquivo");
            return false;
        }

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

        // O FINGERPRINT, sobre o que acabou de ser lido, contra o que o loader carregou. Magic,
        // versao, indice e limites ja passaram — e nada disso distingue um recook que trocou as
        // UVs mantendo as contagens.
        const FOmmContentHash Lido = OmmHashGeometry(
            _Out.TexCoords.data(), sizeof(f32) * 2, Entry.VertexCount,
            _Out.Indices.data(), Entry.IndexCount);
        if (Lido != Source.ContentHash) {
            LogError("[OMM] Rebake: a geometria no .smesh nao e a que esta nos buffers de GPU (o "
                     "arquivo foi recozinhado desde o load). Recarregue a cena");
            _Out.TexCoords.clear();
            _Out.Indices.clear();
            return false;
        }
        return true;
    }
}
