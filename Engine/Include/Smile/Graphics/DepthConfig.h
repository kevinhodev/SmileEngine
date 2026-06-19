#pragma once

#include <d3d12.h>

// Configuracao central de profundidade (Reverse-Z).
//
// Reverse-Z mapeia near->1, far->0. Combinado com depth float (D32_FLOAT) a precisao
// fica ~uniforme em world-space — mata z-fighting distante e limpa a normal reconstruida
// do GTAO (as "manchas distantes residuais"). So a CAMERA PRINCIPAL (perspectiva) usa
// Reverse-Z; as CSM sao ortograficas (depth ja linear, sem ganho) e permanecem forward-Z.
//
// IMPORTANTE: manter kReverseZ em sincronia com SMILE_REVERSE_Z em
// Shaders/Common/DepthConfig.hlsli (flip os dois + rebuild p/ comparar A/B).
namespace Smile {
    inline constexpr bool  kReverseZ   = true;
    inline constexpr float kClearDepth = kReverseZ ? 0.0f : 1.0f; // far plane

    // Funcoes de comparacao da cena (perspectiva). Forward-Z usa LESS/LESS_EQUAL;
    // Reverse-Z inverte para GREATER/GREATER_EQUAL. NAO usar nas CSM (ficam LESS).
    inline constexpr D3D12_COMPARISON_FUNC kDepthFuncLess =
        kReverseZ ? D3D12_COMPARISON_FUNC_GREATER : D3D12_COMPARISON_FUNC_LESS;
    inline constexpr D3D12_COMPARISON_FUNC kDepthFuncLessEqual =
        kReverseZ ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
}
