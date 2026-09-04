#pragma once

// As entradas de bake de micro-map que a cena RETEM para poder rebakear em runtime.
//
// Por que existe: o gate da Fase 2 exige que, mudada qualquer entrada do bake, o
// FOpacityMicromapData seja substituido e os BLAS afetados reconstruidos. Antes disso o runtime
// nao guardava nada do que o bake consome — o FGpuMesh so tem os buffers de GPU e o
// FTextureCPUData e local do import —, entao a unica saida honesta era RECUSAR as edicoes que
// invalidavam o micro-map. Este store e o que troca a recusa por reconstrucao.
//
// O que ele guarda, e por que e pouco:
//
//   GEOMETRIA — apenas UVs e indices. O baker nao le posicao nem normal (ver FOmmGeometry), e o
//   teste do OMM prova, na Bistro inteira, que o bake a partir desta entrada compacta e identico
//   byte a byte ao bake a partir da malha completa. Sao 8 B por vertice e 4 B por indice, contra
//   os 32 B do Vertex, e SO para malhas que participam de algum par mascarado.
//
//   TEXTURA — apenas o CAMINHO, e nao os pixels. A textura de alpha e relida do disco no rebake,
//   que e uma operacao manual e rara; guardar o BC7 de folhagem residente custaria dezenas de MB
//   para um caminho que roda quando o usuario mexe num slider.
//
// Nao e cache de conveniencia: sem uma entrada aqui, o rebake FALHA e diz qual peca faltou. Um
// rebake silenciosamente pulado devolveria exatamente o micro-map obsoleto que a fase existe para
// impedir.

#include "Smile/Core/Types.h"
#include "Smile/Graphics/RayTracing/OpacityMicromap.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Smile {
    struct FMesh;
    class FGpuMesh;
    class FTexture;

    class FOmmBakeInputStore {
    public:
        // Retem UV + indices de uma malha. Idempotente: chamar de novo para a mesma malha nao
        // duplica nada, que e o caso comum — varios pares (malha, material) compartilham a malha.
        void RetainMesh(const FGpuMesh* Mesh, const FMesh& Source);

        // De onde a textura veio, ja resolvido em caminho absoluto, com as flags de decodificacao
        // que o load original usou. Sem elas o rebake decodificaria o mesmo arquivo de outro jeito.
        void RetainTexturePath(const FTexture* Texture, std::wstring Path, bool IsNormalMap,
                               bool sRGB);

        void Clear();

        // Geometria retida, apontando para dentro do store. Invalida (Valid() falso) quando a
        // malha nao foi retida.
        FOmmGeometry GeometryOf(const FGpuMesh* Mesh) const;

        struct FTextureSource {
            std::wstring Path;
            bool IsNormalMap = false;
            bool sRGB        = false;
        };
        // nullptr quando a textura nao tem procedencia conhecida — o albedo do proxy do terreno,
        // por exemplo, e gerado e nao veio de arquivo nenhum.
        const FTextureSource* TextureSourceOf(const FTexture* Texture) const;

        u32 MeshCount()     const { return static_cast<u32>(Meshes_.size()); }
        u32 TextureCount()  const { return static_cast<u32>(Textures_.size()); }
        // Memoria que a cena passa a segurar por causa disto. Publicada porque e um custo novo e
        // permanente: sem numero, "retivemos so o necessario" e afirmacao sem verificacao.
        u64 RetainedBytes() const { return RetainedBytes_; }

    private:
        struct FRetainedMesh {
            std::vector<f32> TexCoords; // compactadas: 2 floats por vertice
            std::vector<u32> Indices;
        };

        std::unordered_map<const FGpuMesh*, FRetainedMesh>  Meshes_;
        std::unordered_map<const FTexture*, FTextureSource> Textures_;
        u64 RetainedBytes_ = 0;
    };
}
