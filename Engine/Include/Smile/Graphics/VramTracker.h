#pragma once

#include <d3d12.h>
#include <array>
#include <vector>
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
        Terrain,        // heightmap + grids de chunk
        Misc,           // chuva, picking, debug — o resto instrumentado
        Count
    };

    struct FVramSnapshot {
        std::array<u64, static_cast<size_t>(EVramCategory::Count)> Bytes{};
        u64 TotalTracked = 0;
        // Recursos rastreados vivos. Enquanto a engine so cria COMMITTED, este numero e
        // tambem a contagem de HEAPS do driver — um committed resource e um heap. E a
        // medida de fragmentacao que decide se D3D12MA ou um pool tem cliente; no dia em
        // que existir placed resource ele deixa de valer como contagem de heap.
        u32 LiveResources = 0;
    };

    // Uma linha do breakdown POR RECURSO: a soma de todos os recursos que compartilham o mesmo
    // rotulo dentro de uma categoria (ver Register). O que sobra de uma categoria — recursos
    // registrados sem rotulo — nao aparece aqui; e `Snapshot().Bytes[cat]` menos o que saiu.
    struct FVramLabelEntry {
        EVramCategory Category;
        const char*   Label;
        u64           Bytes;
    };

    // Registro central, thread-safe. Register() ignora heaps UPLOAD/READBACK (moram em
    // system memory, nao em VRAM) e desregistra sozinho quando o recurso morre
    // (ID3DDestructionNotifier) — recurso recriado em resize nao conta dobrado.
    namespace VramTracker {
        // Label e OPCIONAL e abre o breakdown por recurso dentro da categoria. Tem que ser um
        // literal (ou qualquer string de vida estatica): o tracker guarda o ponteiro, nao copia.
        // Recursos que compartilham o rotulo somam — e o que se quer p/ ping-pong e pools, onde
        // "ReSTIR GI · Res0" deve aparecer como uma linha so com as duas copias juntas.
        void Register(ID3D12Resource* Resource, EVramCategory Category,
                      const char* Label = nullptr);
        FVramSnapshot Snapshot();
        // Ordenada por bytes decrescente. Alimenta o log e as linhas expansiveis da janela de
        // Estatisticas — as duas leem a MESMA fonte, entao nao ha como uma divergir da outra.
        std::vector<FVramLabelEntry> LabelBreakdown();
        const char* CategoryName(EVramCategory Category); // rotulo pt-BR pro editor

        // Despeja o mesmo breakdown da janela de Estatisticas no log, em ordem decrescente.
        // Existe porque a janela e o UNICO consumidor do Snapshot: sem isto, comparar VRAM entre
        // duas builds depende de alguem ler numero de tela e lembrar do valor anterior.
        //
        // LocalUsageBytes = uso reportado pelo DXGI (0 = nao consultado). Ele NAO e a soma das
        // categorias: entram driver, descriptor heaps, swapchain, upload heaps e tudo que e
        // alocado por SDK de terceiro, que nunca passa por Register().
        //
        // O maior movedor dessa diferenca e o DENOISER: ligar/desligar muda "nao rastreado" em
        // centenas de MB. O NRD nao e o caso — os pools permanente e transiente dele saem do
        // GpuResources e entram em GI; e o caminho Streamline (DLSS-RR) que aloca por fora.
        // Por isso a linha separa as duas medidas em vez de so imprimir um total: comparar
        // builds pelas CATEGORIAS e valido, pelo numero do DXGI nao e.
        void LogBreakdown(u64 LocalUsageBytes);
    }
}
