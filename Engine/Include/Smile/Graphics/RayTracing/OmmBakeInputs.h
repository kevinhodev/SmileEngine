#pragma once

// De onde saem as entradas de bake de micro-map quando e preciso REBAKEAR em runtime.
//
// Por que existe: o gate da Fase 2 exige que, mudada qualquer entrada do bake, o
// FOpacityMicromapData seja substituido e os BLAS afetados reconstruidos. O runtime nao guarda
// nada do que o bake consome — o FGpuMesh so tem os buffers de GPU e o FTextureCPUData e local do
// import —, e sem uma resposta para "de onde tiro isso de novo" a unica saida era RECUSAR as
// edicoes que invalidavam o micro-map.
//
// O que ele guarda e PROCEDENCIA, nao dado:
//
//   MALHA — o arquivo .smesh e o indice da entrada. A geometria de bake (UVs + indices) e lida
//   sob demanda. A primeira versao disto retinha os proprios UVs e indices, e isso tinha dois
//   defeitos: pagava memoria residente por uma operacao manual e rara, e so podia reter as malhas
//   que ja eram MASCARADAS no import — entao ligar Alpha Test num material opaco depois nao tinha
//   com que bakear. Guardando procedencia, cabe reter TODAS as malhas por ~40 B cada, e a
//   transicao opaco -> masked passa a funcionar.
//
//   TEXTURA — o caminho do arquivo, com as flags de decodificacao do load original. Mesma razao:
//   guardar o BC7 de folhagem residente custaria dezenas de MB por um caminho que roda quando
//   alguem mexe num slider.
//
// Nao e cache de conveniencia: sem procedencia, o rebake FALHA e diz qual peca faltou. Um rebake
// silenciosamente pulado devolveria exatamente o micro-map obsoleto que a fase existe para impedir.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    class FGpuMesh;
    class FTexture;

    class FOmmBakeInputStore {
    public:
        // De qual arquivo cozido, e qual entrada dentro dele, a geometria desta malha veio.
        // Idempotente por malha.
        void RetainMeshSource(const FGpuMesh* Mesh, std::wstring MeshFilePath, u32 MeshIndex);

        // De onde a textura veio, ja resolvido em caminho absoluto, com as flags de decodificacao
        // que o load original usou. Sem elas o rebake decodificaria o mesmo arquivo de outro jeito.
        void RetainTexturePath(const FTexture* Texture, std::wstring Path, bool IsNormalMap,
                               bool sRGB);

        void Clear();

        // Geometria de bake carregada sob demanda. Os vetores sao do CHAMADOR: a view aponta para
        // dentro deles, e nada disso fica residente depois.
        struct FLoadedGeometry {
            std::vector<f32> TexCoords; // compactadas, 2 floats por vertice
            std::vector<u32> Indices;

            FOmmGeometry View() const {
                FOmmGeometry G;
                if (TexCoords.empty() || Indices.empty()) return G;
                G.TexCoords      = TexCoords.data();
                G.TexCoordStride = sizeof(f32) * 2;
                G.VertexCount    = static_cast<u32>(TexCoords.size() / 2);
                G.Indices        = Indices.data();
                G.IndexCount     = static_cast<u32>(Indices.size());
                return G;
            }
        };

        // Le do .smesh as UVs e os indices desta malha. Falso quando a malha nao tem procedencia
        // (criada em runtime) ou o arquivo nao bate com o que foi registrado — e nesse caso quem
        // chamou tem de recusar o rebake, nunca seguir com o micro-map antigo.
        bool LoadGeometry(const FGpuMesh* Mesh, FLoadedGeometry& Out) const;

        struct FTextureSource {
            std::wstring Path;
            bool IsNormalMap = false;
            bool sRGB        = false;
        };
        // nullptr quando a textura nao tem procedencia conhecida — o albedo do proxy do terreno,
        // por exemplo, e gerado e nao veio de arquivo nenhum.
        const FTextureSource* TextureSourceOf(const FTexture* Texture) const;

        u32 MeshCount()    const { return static_cast<u32>(Meshes_.size()); }
        u32 TextureCount() const { return static_cast<u32>(Textures_.size()); }
        // Memoria que a cena segura por causa disto. Publicada porque e um custo novo; guardando
        // procedencia em vez de dado, ela e da ordem de dezenas de bytes por malha, e o numero
        // existe justamente para que isso continue verdadeiro.
        u64 RetainedBytes() const;

    private:
        struct FMeshSource {
            std::wstring FilePath;
            u32          MeshIndex = 0;
        };

        std::unordered_map<const FGpuMesh*, FMeshSource>    Meshes_;
        std::unordered_map<const FTexture*, FTextureSource> Textures_;
    };
}
