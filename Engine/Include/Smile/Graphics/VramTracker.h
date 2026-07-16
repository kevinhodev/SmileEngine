#pragma once

#include <d3d12.h>
#include <array>
#include "Smile/Core/Types.h"

namespace Smile {
    // Categorias do breakdown de VRAM (estilo Flax: cada recurso registra o tamanho na
    // criacao). A diferenca pro CurrentUsage do DXGI vira "nao rastreado" no editor —
    // driver, descriptor heaps, swapchain, upload heaps e sistemas nao instrumentados.
    enum class EVramCategory : u8 {
        Geometry,       // VB/IB das meshes da cena
        SceneTextures,  // texturas de material, skybox, IBL/HDR
        RenderTargets,  // G-buffer, HDR, depth, historicos de temporal, AO
        Shadows,        // CSM do sol + atlas/cubemaps de luzes locais
        RaytracingAS,   // BLAS/TLAS
        GI,             // DDGI, ReSTIR, reflexoes, NRD
        Sky,            // atmosfera, nuvens (noises 3D/weather), estrelas, sun shafts
        Water,          // FFT + superficie
        Misc,           // chuva, picking, debug — o resto instrumentado
        Count
    };

    struct FVramSnapshot {
        std::array<u64, static_cast<size_t>(EVramCategory::Count)> Bytes{};
        u64 TotalTracked = 0;
    };

    // Registro central, thread-safe. Register() ignora heaps UPLOAD/READBACK (moram em
    // system memory, nao em VRAM) e desregistra sozinho quando o recurso morre
    // (ID3DDestructionNotifier) — recurso recriado em resize nao conta dobrado.
    namespace VramTracker {
        void Register(ID3D12Resource* Resource, EVramCategory Category);
        FVramSnapshot Snapshot();
        const char* CategoryName(EVramCategory Category); // rotulo pt-BR pro editor
    }
}
