#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <memory>
#include <vector>

namespace Smile {
    struct FMesh;
    struct FTextureCPUData;
    class  FGpuMesh;
    class  FMaterial;
    class  FTexture;

    // As ENTRADAS do bake, na forma em que o runtime consegue reconhece-las.
    //
    // Por que existe: o micro-map e derivado de malha + textura de alpha + fator de alpha +
    // cutoff, e o unico guarda que havia comparava DOIS desses quatro (BakedBaseAlpha e
    // BakedAlphaCutoff). Trocar a textura de albedo de um material alpha-tested no editor
    // — ApplySlotTexture, slot 0 — passava por baixo desse guarda e deixava o micro-map
    // classificando microtriangulos pelo alpha da textura ANTIGA. Nao ha excecao, nao ha erro:
    // ha folhagem com recorte errado, que e quase invisivel numa comparacao estatica.
    //
    // Os campos sao a IDENTIDADE das entradas, e nao o conteudo delas: o runtime nao guarda os
    // vertices da malha nem os pixels da textura depois do load, entao "a mesma malha" e "a mesma
    // textura" so podem ser afirmadas por ponteiro. Isso e suficiente para o que muda em runtime
    // — trocar de textura troca o ponteiro — e nao pretende cobrir edicao do ASSET em disco.
    struct FOmmInputKey {
        const FGpuMesh* Mesh         = nullptr;
        const FTexture* AlphaTexture = nullptr;
        f32             BaseAlpha    = 1.0f;
        f32             AlphaCutoff  = 0.5f;
        // Se o material AINDA e alpha-tested. Desligar o alpha test tambem invalida: o BLAS
        // exigido deixa de ser o de micro-map e passa a ser o opaco.
        u32             AlphaTest    = 0;

        // Chave viva de um par malha/material. Devolve a chave vazia para argumentos nulos, que
        // nunca casa com a de um bake real.
        static FOmmInputKey From(const FGpuMesh* Mesh, const FMaterial* Material);

        friend bool operator==(const FOmmInputKey& A, const FOmmInputKey& B) {
            return A.Mesh == B.Mesh && A.AlphaTexture == B.AlphaTexture &&
                   A.BaseAlpha == B.BaseAlpha && A.AlphaCutoff == B.AlphaCutoff &&
                   A.AlphaTest == B.AlphaTest;
        }
        friend bool operator!=(const FOmmInputKey& A, const FOmmInputKey& B) { return !(A == B); }
    };

    // Resultado independente do baker: os ponteiros devolvidos pelo OMM SDK deixam de ser
    // validos ao destruir o BakeResult, entao tudo que o D3D12 consumira e copiado para ca.
    struct FOpacityMicromapData {
        std::vector<u8> ArrayData;
        std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> Descs;
        std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> Histogram;
        std::vector<u8> Indices;
        DXGI_FORMAT IndexFormat  = DXGI_FORMAT_UNKNOWN;
        u32         IndexStride  = 0;
        u32         TriangleCount = 0;
        f32         BakedBaseAlpha = 1.0f;
        f32         BakedAlphaCutoff = 0.5f;
        // Triangulos que ficaram FORA do bake por estourar o teto de workload e sairam com o
        // indice especial FULLY_UNKNOWN_OPAQUE: corretos (o alpha-test exato resolve), mas sem
        // nenhum ganho de micro-map. Zero no bake normal; TriangleCount quando nem a divisao
        // coube. Publicado porque e a grandeza que diz quanto da folhagem o OMM nao cobre.
        u32         WorkloadExcludedTriangles = 0;

        // O indice OMM do triangulo, com sinal: >= 0 aponta para um micro-map do array, -1..-4
        // sao os indices especiais do D3D12. Le no formato que o bake escolheu (8/16/32 bits),
        // estendendo o sinal — o -4 de 16 bits e 0xFFFC, nao 65532.
        i32 IndexOf(u32 Triangle) const;

        // Um bake inteiramente uniforme pode usar apenas indices especiais (-1..-4), sem
        // ArrayData/Descs. Continua sendo OMM valido e ainda elimina o alpha-test por hardware.
        bool Valid() const {
            return TriangleCount > 0 && IndexStride > 0 &&
                   Indices.size() == static_cast<size_t>(TriangleCount) * IndexStride &&
                   ((ArrayData.empty() && Descs.empty() && Histogram.empty()) ||
                    (!ArrayData.empty() && !Descs.empty() && !Histogram.empty()));
        }

        // As entradas com que ESTE bake foi feito. Carimbada pelo importador, que e quem conhece
        // a malha e o material de GPU; o baker nao os conhece. Uma chave vazia (nunca carimbada)
        // nao casa com nenhuma chave viva, e o consumidor recusa — falhar alto e a direcao certa
        // para um caminho de criacao novo que esqueca de carimbar.
        FOmmInputKey BakedInputs;

        // Este micro-map ainda descreve o par malha/material de agora?
        //
        // Substituiu um Matches(BaseAlpha, AlphaCutoff) que comparava DOIS dos quatro inputs. Ver
        // FOmmInputKey.
        bool MatchesInputs(const FOmmInputKey& Live) const { return BakedInputs == Live; }

        // Este micro-map serve de destino para uma REVERSAO do material que aponta para
        // `CurrentAlbedo`? Ou seja: desfazer a edicao devolve um material MASCARADO (as entradas
        // que este bake descreve), ou o alpha test tem de ser desligado por nao haver micro-map
        // que sirva?
        //
        // Mora aqui, e nao no chamador, porque a resposta e consultada em dois momentos que nao
        // podem divergir: antes da reconstrucao dos BLAS, para saber se o BLAS OPACO daquelas
        // malhas passa a ser exigido, e depois dela, para aplicar a reversao. Duas copias da regra
        // divergem no dia em que uma das duas ganhar um caso a mais, e a divergencia apareceria
        // como "BLAS opaco ausente" derrubando a render thread.
        bool CanRestoreAlphaTestFor(const FTexture* CurrentAlbedo) const {
            return BakedInputs.AlphaTest != 0 && BakedInputs.AlphaTexture == CurrentAlbedo;
        }
    };

    // A GEOMETRIA que o bake consome, e nada alem dela.
    //
    // Sao dois arrays: as UVs (com stride, porque no FMesh elas vivem dentro do Vertex) e os
    // indices. Posicao e normal NAO entram — o micro-map e definido no espaco de textura do
    // triangulo. Isso importa para o rebake em runtime: reter as entradas de uma malha mascarada
    // custa 8 B por vertice e 4 B por indice, e nao os 32 B do Vertex inteiro.
    struct FOmmGeometry {
        const f32* TexCoords     = nullptr;
        u32        TexCoordStride = 0; // bytes entre UVs consecutivas
        u32        VertexCount   = 0;
        const u32* Indices       = nullptr;
        u32        IndexCount    = 0;

        bool Valid() const {
            return TexCoords && Indices && VertexCount > 0 && IndexCount > 0 &&
                   (IndexCount % 3) == 0 && TexCoordStride >= sizeof(f32) * 2;
        }
    };

    class FOpacityMicromapBaker {
    public:
        // Baker CPU oficial NVIDIA, em 4-state. AlphaTexture nula representa uma textura branca;
        // BaseAlpha e multiplicado no plano antes do bake para casar com AlphaTestPass no HLSL.
        // Falha lanca: OMM e requisito da engine, portanto nao existe retorno para o BLAS antigo.
        static std::shared_ptr<FOpacityMicromapData> Bake(const FOmmGeometry& Geometry,
                                                          const FTextureCPUData* AlphaTexture,
                                                          f32 BaseAlpha,
                                                          f32 AlphaCutoff,
                                                          bool EnableInternalThreads = true,
                                                          u32 InternalThreadCount = 0);

        // Conveniencia para quem tem a malha inteira em maos (o import). Extrai a FOmmGeometry e
        // delega — o bake resultante e o MESMO, e o teste prova isso comparando byte a byte.
        static std::shared_ptr<FOpacityMicromapData> Bake(const FMesh& Mesh,
                                                          const FTextureCPUData* AlphaTexture,
                                                          f32 BaseAlpha,
                                                          f32 AlphaCutoff,
                                                          bool EnableInternalThreads = true,
                                                          u32 InternalThreadCount = 0);

        // A geometria de bake de uma malha, apontando PARA DENTRO dela. Nao copia: o chamador
        // garante que a malha sobreviva ao uso.
        static FOmmGeometry GeometryOf(const FMesh& Mesh);
    };
}
