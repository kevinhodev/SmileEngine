#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/VramTracker.h"

// Fabrica de recursos D3D12. FUNIL FECHADO: nao existe mais nenhum CreateCommittedResource
// nem D3D12_HEAP_TYPE_* fora deste arquivo (a unica excecao e o VramTracker, que INSPECIONA
// o heap de um recurso pronto para decidir se o rastreia — leitura, nao criacao).
//
// Havia 132 CreateCommittedResource espalhados por 48 arquivos, cada um remontando
// D3D12_HEAP_PROPERTIES + D3D12_RESOURCE_DESC campo a campo, mais uma duzia de copias locais
// do mesmo helper. Alem do ruido, o boilerplate abria tres classes de bug silencioso: campo
// do desc esquecido (Layout/SampleDesc zerados tem significado), VramTracker::Register
// esquecido (o recurso some do breakdown de VRAM do editor), e desc COMPARTILHADO entre duas
// criacoes com um `Desc.Width =` no meio — correto enquanto ninguem mexesse na ordem.
//
// Fechar o funil e o que torna qualquer politica futura (pool de reciclagem por hash do desc,
// D3D12MA, placed resource) uma mudanca em UM lugar em vez de cinquenta.
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

    // === Criacao que pode falhar sem derrubar a engine ============================
    // Devolvem HRESULT em vez de lancar, e deixam Out INTACTO em falha. Nao sao um "por via
    // das duvidas": existem para o recurso de FEATURE OPCIONAL, onde falhar significa
    // desligar a feature e seguir em resolucao nativa — o que os upscalers (FSR/DLSS/DLSS-RR)
    // e o timer da NVAPI ja faziam a mao antes da fachada existir. Usar a versao que lanca
    // nesses quatro pontos trocaria "upscaler indisponivel" por um crash.
    //
    // Para todo o resto prefira as versoes acima: falha de alocacao em recurso obrigatorio e
    // OOM, e engolir o HRESULT so adia o crash para um lugar pior de diagnosticar.
    HRESULT TryCreateTex2D(ID3D12Device* Device, ComPtr<ID3D12Resource>& Out,
                           u32 Width, u32 Height, DXGI_FORMAT Format,
                           D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES InitialState,
                           EVramCategory Category, const D3D12_CLEAR_VALUE* Clear = nullptr,
                           u32 MipLevels = 1, u32 ArraySize = 1, const char* Label = nullptr);

    HRESULT TryCreateBuffer(ID3D12Device* Device, ComPtr<ID3D12Resource>& Out, u64 Bytes,
                            D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES InitialState,
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
