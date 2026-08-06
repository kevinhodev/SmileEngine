#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/VramTracker.h"

// Fabrica de recursos D3D12.
//
// Havia 132 CreateCommittedResource espalhados por 48 arquivos, cada um remontando
// D3D12_HEAP_PROPERTIES + D3D12_RESOURCE_DESC campo a campo — e 5 copias locais do mesmo
// helper de textura (AmbientOcclusion, DDGI, NrdDenoiser, TemporalMotionVectors, mais o
// upload buffer do TemporalMotionVectors). Alem do ruido, o boilerplate abria duas classes
// de bug silencioso: campo do desc esquecido (Layout/SampleDesc zerados tem significado) e
// VramTracker::Register esquecido, que some com o recurso do breakdown de VRAM do editor.
//
// Aqui o registro no VramTracker e parte da criacao, nao um passo que cada autor precisa
// lembrar: quem cria em DEFAULT heap escolhe a categoria e pronto. Uploads e readbacks nao
// entram no tracker de proposito (moram em system memory — ver VramTracker.h).
//
// Nao substitui caminhos com necessidade propria (reservados, placed, aliasing): o objetivo
// e cobrir a forma comum, nao virar uma camada de abstracao sobre o D3D12.
namespace Smile::GpuResources {

    // === Descritores ==============================================================
    // Uteis quando o chamador precisa do desc antes de criar (footprints de copia, ou
    // criacao por um caminho proprio). Os Create* abaixo usam estes por dentro.

    D3D12_RESOURCE_DESC Tex2DDesc(u32 Width, u32 Height, DXGI_FORMAT Format,
                                  D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE,
                                  u32 MipLevels = 1, u32 ArraySize = 1);

    D3D12_RESOURCE_DESC Tex3DDesc(u32 Width, u32 Height, u32 Depth, DXGI_FORMAT Format,
                                  D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE,
                                  u32 MipLevels = 1);

    D3D12_RESOURCE_DESC BufferDesc(u64 Bytes,
                                   D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE);

    // === Criacao em DEFAULT heap (VRAM) ===========================================
    // Lancam via SMILE_HR em falha, como o resto da engine. Category alimenta o breakdown
    // de VRAM do editor — escolha a que descreve o SUBSISTEMA dono, nao o tipo do recurso.

    ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* Device, u32 Width, u32 Height,
                                       DXGI_FORMAT Format, D3D12_RESOURCE_FLAGS Flags,
                                       D3D12_RESOURCE_STATES InitialState,
                                       EVramCategory Category,
                                       const D3D12_CLEAR_VALUE* Clear = nullptr,
                                       u32 MipLevels = 1, u32 ArraySize = 1,
                                       // Rotulo p/ o breakdown por recurso (ver VramTracker.h).
                                       const char* Label = nullptr);

    ComPtr<ID3D12Resource> CreateTex3D(ID3D12Device* Device, u32 Width, u32 Height, u32 Depth,
                                       DXGI_FORMAT Format, D3D12_RESOURCE_FLAGS Flags,
                                       D3D12_RESOURCE_STATES InitialState,
                                       EVramCategory Category, u32 MipLevels = 1);

    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* Device, u64 Bytes,
                                        D3D12_RESOURCE_FLAGS Flags,
                                        D3D12_RESOURCE_STATES InitialState,
                                        EVramCategory Category, const char* Label = nullptr);

    // === Upload heap mapeado de forma persistente =================================
    // A forma mais repetida da engine: 48 ocorrencias em 33 arquivos, quase sempre um CB por
    // frame em voo. O Map e feito UMA vez, com range de leitura vazio (a CPU so escreve).
    //
    // SliceBytes e arredondado para 256 B quando ForConstantBuffer (exigencia de alinhamento
    // de CBV do D3D12). Sem isso, o slice 1 em diante cai fora do alinhamento e a criacao da
    // view falha — bug classico que aparecia como "so o primeiro frame esta certo".
    struct FUploadBuffer {
        ComPtr<ID3D12Resource> Resource;
        u8*                    Mapped     = nullptr;
        u64                    SliceBytes = 0; // ja alinhado
        u32                    SliceCount = 0;

        u8* Slice(u32 Index) const {
            return Mapped + static_cast<size_t>(Index) * SliceBytes;
        }
        D3D12_GPU_VIRTUAL_ADDRESS Address(u32 Index) const {
            return Resource->GetGPUVirtualAddress() + static_cast<u64>(Index) * SliceBytes;
        }
        ID3D12Resource* Get() const { return Resource.Get(); }
        explicit operator bool() const { return Mapped != nullptr; }
    };

    FUploadBuffer CreateUploadBuffer(ID3D12Device* Device, u64 SliceBytes, u32 SliceCount = 1,
                                     bool ForConstantBuffer = true);

    // Readback (GPU -> CPU). Nao entra no VramTracker (system memory).
    ComPtr<ID3D12Resource> CreateReadbackBuffer(ID3D12Device* Device, u64 Bytes);

    // === Descritores de view ======================================================
    // Os campos que nao aparecem aqui sao os que ficam no default em 100% dos usos atuais
    // (PlaneSlice, ResourceMinLODClamp). Se um caso precisar deles, monte o desc na mao —
    // o helper existe para a forma comum, nao para cobrir o D3D12 inteiro.

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvTex2D(DXGI_FORMAT Format, u32 MipLevels = 1,
                                             u32 MostDetailedMip = 0);
    D3D12_UNORDERED_ACCESS_VIEW_DESC UavTex2D(DXGI_FORMAT Format, u32 MipSlice = 0);
    D3D12_RENDER_TARGET_VIEW_DESC    RtvTex2D(DXGI_FORMAT Format, u32 MipSlice = 0);

    // StructuredBuffer<T>: Format UNKNOWN + StructureByteStride, como o shader espera.
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvStructuredBuffer(u32 NumElements, u32 StrideBytes,
                                                        u64 FirstElement = 0);
    D3D12_UNORDERED_ACCESS_VIEW_DESC UavStructuredBuffer(u32 NumElements, u32 StrideBytes,
                                                         u64 FirstElement = 0);
}
