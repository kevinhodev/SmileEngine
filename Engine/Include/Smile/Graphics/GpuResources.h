#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/VramTracker.h"

// Fabrica de recursos D3D12. FUNIL FECHADO: o codigo da engine nao cria committed resources
// A implementacao vendorada naturalmente usa a API nativa; o VramTracker apenas INSPECIONA
// o heap de um recurso pronto para decidir se o rastreia (leitura, nao criacao).
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

    // Inicializado pelo FD3D12Device depois de descobrir o Resource Heap Tier. A primeira
    // versao usa os pools DEFAULT do D3D12MA somente em Tier 2. Tier 1 e recursos RT/DS
    // continuam committed: os passes antigos ainda nao garantem Clear/Discard/Copy antes do
    // primeiro uso de todo alvo placed.
    bool InitializeDefaultAllocator(ID3D12Device* Device, IDXGIAdapter* Adapter,
                                    D3D12_RESOURCE_HEAP_TIER HeapTier);
    void ShutdownDefaultAllocator(ID3D12Device* Device);

    struct FMemoryAllocatorStats {
        bool Enabled          = false;
        u32  ActiveResources  = 0;
        u64  BlockBytes       = 0; // heaps reservados pelo D3D12MA
        u64  AllocationBytes  = 0; // ranges ocupados por recursos vivos
    };

    FMemoryAllocatorStats MemoryAllocatorStats();

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

    // === Custo de CRIACAO =========================================================
    // Eixo diferente do VramTracker, que mede RESIDENCIA (quanto esta alocado). Aqui e
    // quanto CUSTA alocar: e a medida que faltava para decidir, com numero em vez de
    // argumento, entre pool de reciclagem, alocar no tamanho maximo, e D3D12MA.
    //
    // Existe porque o funil esta fechado: com 132 call sites isto seria instrumentacao
    // espalhada; com um, e um contador. Custo por criacao = dois steady_clock::now(), e
    // criacao de recurso nunca esta no caminho quente de frame.

    enum class EHeapClass : u8 { Default, Upload, Readback, Count };
    enum class EDefaultCreationPath : u8 { Committed, D3D12MA, Count };

    struct FCreationStats {
        u32 Count [static_cast<size_t>(EHeapClass::Count)]{};
        u64 Bytes [static_cast<size_t>(EHeapClass::Count)]{};
        u64 Nanos [static_cast<size_t>(EHeapClass::Count)]{}; // tempo DENTRO do D3D12

        // Subdivisao do DEFAULT. Count/Bytes/Nanos acima continuam sendo o total e preservam
        // os consumidores existentes; estes campos tornam o A/B do allocator visivel no
        // mesmo log de fase sem misturar fallback committed com placed.
        u32 DefaultPathCount [static_cast<size_t>(EDefaultCreationPath::Count)]{};
        u64 DefaultPathBytes [static_cast<size_t>(EDefaultCreationPath::Count)]{};
        u64 DefaultPathNanos [static_cast<size_t>(EDefaultCreationPath::Count)]{};
    };

    FCreationStats CreationStats();
    void           ResetCreationStats();

    // Loga o delta desde o ultimo ResetCreationStats. Label diz de que janela se trata
    // ("load da cena", "render scale"), porque o mesmo contador serve as duas.
    void LogCreationStats(const char* Label);

    // Loga o delta desde um snapshot, SEM zerar o contador global — e o que permite medir
    // uma fase de dentro de outra. Existe porque a primeira leitura desta instrumentacao
    // comparou o total do commit inteiro contra o tempo de UMA fase (uploadTex) e concluiu
    // errado: o contador tambem cobria mesh, terreno, BLAS/TLAS e GI. Com fase aninhada, o
    // numero de cada uma sai direto e a conta errada deixa de ser possivel.
    // Devolve o delta desde um snapshot, sem logar. As fases somam neste para o chamador
    // fechar a conta contra o total.
    FCreationStats CreationDelta(const FCreationStats& Since);

    void LogCreationDelta(const char* Label, const FCreationStats& Since);

    // Loga a fase E soma o delta dela em InOutSum, para o chamador fechar a conta com o
    // LogCreationUnattributed abaixo sem manter duas listas em paralelo.
    void AccumulatePhase(FCreationStats& InOutSum, const char* Label,
                         const FCreationStats& Since);

    // Loga o que a soma das fases NAO explica (total da janela menos o que elas somaram).
    // Existe porque a primeira versao das fases somou 487 de 492 recursos: paginas de CB de
    // material e o RecreateObjectCB/HiZ nascem entre os snapshots. Uma fase nova esquecida
    // no futuro aparece aqui em vez de sumir — e o que uma lista de fases a dedo nao garante.
    void LogCreationUnattributed(const char* Label, const FCreationStats& SumOfPhases);

    // === Captura de descritores (diagnostico) =====================================
    // Grava os descritores criados enquanto ligada, para o Tools/AllocBench REPRODUZIR o
    // conjunto real em vez de um representativo. A primeira versao do benchmark inventou um
    // conjunto de "~600 MB" que na verdade somava 787 MB — 30% acima do resize real —, o que
    // manteve valido o sinal (placed e muito mais barato) mas envenenou qualquer coeficiente
    // por MB extraido dele.
    //
    // Desligada por padrao e sem custo quando desligada (um bool checado por criacao).
    //
    // Ha DUAS janelas de captura na engine (o load da cena e o resize), e elas compartilham
    // um vetor so. Por isso ligar devolve se FOI ESTA CHAMADA que ligou: se outra janela ja
    // estava aberta, esta nao mexe em nada e devolve false — sem isso, uma captura aninhada
    // limparia o vetor da externa e a desligaria no meio, produzindo um conjunto que parece
    // legitimo e nao e. Quem recebeu false nao deve desligar nem dumpar.
    //
    // Hoje as duas janelas nao se aninham (RecreateInternalTargets so entra por Resize e por
    // ApplyRenderScale, nenhum deles chamado de dentro do commit da cena). O guard existe
    // para o dia em que isso mudar, porque o modo de falha e silencioso.
    bool SetDescCapture(bool Enabled);
    bool DumpCapturedDescs(const char* Path); // texto; false se nao abriu ou nada capturado

    // Fecha a captura mesmo se SMILE_HR lancar no meio de um load/resize. Complete() grava o
    // conjunto apenas no caminho de sucesso; o destrutor so desliga e descarta a janela
    // parcial, impedindo uma falha de deixar a captura global ligada para sempre.
    class FDescCaptureSession {
    public:
        explicit FDescCaptureSession(const char* Path);
        ~FDescCaptureSession() noexcept;

        FDescCaptureSession(const FDescCaptureSession&) = delete;
        FDescCaptureSession& operator=(const FDescCaptureSession&) = delete;

        bool OwnsCapture() const { return Owns; }
        bool Complete();

    private:
        std::string Path;
        bool        Owns = false;
    };

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
