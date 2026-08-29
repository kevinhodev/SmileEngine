#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <memory>
#include <vector>

namespace Smile {
    struct FMesh;
    struct FTextureCPUData;

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

        // Um bake inteiramente uniforme pode usar apenas indices especiais (-1..-4), sem
        // ArrayData/Descs. Continua sendo OMM valido e ainda elimina o alpha-test por hardware.
        bool Valid() const {
            return TriangleCount > 0 && IndexStride > 0 &&
                   Indices.size() == static_cast<size_t>(TriangleCount) * IndexStride &&
                   ((ArrayData.empty() && Descs.empty() && Histogram.empty()) ||
                    (!ArrayData.empty() && !Descs.empty() && !Histogram.empty()));
        }

        bool Matches(f32 BaseAlpha, f32 AlphaCutoff) const {
            return BakedBaseAlpha == BaseAlpha && BakedAlphaCutoff == AlphaCutoff;
        }
    };

    class FOpacityMicromapBaker {
    public:
        // Baker CPU oficial NVIDIA, em 4-state. AlphaTexture nula representa uma textura branca;
        // BaseAlpha e multiplicado no plano antes do bake para casar com AlphaTestPass no HLSL.
        // Falha lanca: OMM e requisito da engine, portanto nao existe retorno para o BLAS antigo.
        static std::shared_ptr<FOpacityMicromapData> Bake(const FMesh& Mesh,
                                                          const FTextureCPUData* AlphaTexture,
                                                          f32 BaseAlpha,
                                                          f32 AlphaCutoff);
    };
}
