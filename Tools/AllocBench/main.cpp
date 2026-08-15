// SmileAllocBench — quanto do custo de criar recurso e o HEAP IMPLICITO, e quanto disso um
// sub-alocador de verdade devolve?
//
// A instrumentacao da engine mediu, na Bistro, 0,013 ms por objeto de recurso, 0,289 ms de
// overhead por HEAP e 0,042 ms/MB de commit de pagina. O termo por heap e o alvo: placed
// resource nao cria heap. Mas "placed economiza X" era teto teorico, nao medida de biblioteca.
//
// Mesmos descritores, caminhos comparaveis:
//
//   1. committed            — o que a engine faz hoje (recurso + heap implicito por recurso)
//   2. committed NOT_ZEROED — (1) mais D3D12_HEAP_FLAG_CREATE_NOT_ZEROED. Existe porque os
//                             flags recomendados do D3D12MA JA incluem esse comportamento:
//                             sem esta linha, todo ganho do zeramento evitado seria creditado
//                             a sub-alocacao, que nao tem nada com isso. E, se for aqui que
//                             mora o ganho, e uma linha na engine em vez de uma dependencia.
//   3. placed/heap pronto   — heap unico criado ANTES do cronometro; so o objeto de recurso
//                             entra na conta. E o TETO do que qualquer sub-alocador pode
//                             economizar, porque nenhum cria menos que zero heap.
//   4. placed/heap dentro   — heap grande criado DENTRO do cronometro: a ordem de grandeza de
//                             UM heap grande contra N pequenos.
//   5-8. D3D12MA            — a biblioteca, em duas dimensoes cruzadas:
//        FRIO   = allocator recem-criado, sem blocos. E o LOAD DE CENA: os heaps de 64 MiB
//                 ainda precisam nascer. Compara com (4).
//        QUENTE = allocator que ja rodou uma passada. Separar frio de quente e obrigatorio:
//                 medir um e reportar o outro foi a classe de erro que derrubou tres das
//                 quatro conclusoes da primeira rodada desta medicao.
//        CreateResource      = o caminho canonico da lib.
//        AllocateMemory+alias = o caminho que a fachada da engine usaria (ver a validacao do
//                 destruction notifier no fim). Os dois estao aqui porque, se divergirem, e o
//                 numero que decide se vale refatorar o tipo de retorno da GpuResources.
//   9. D3D12MA pool retido  — custom pool com MinBlockCount, que segura os blocos "even if
//                 they stay empty". Entrou depois da primeira execucao, que mostrou o
//                 "quente" custando quase o mesmo que criar o heap na hora: sem MinBlockCount
//                 a lib DEVOLVE o heap assim que o bloco esvazia, e a passada seguinte repaga
//                 o commit de pagina. E o unico caminho em que quente e quente de verdade —
//                 ao preco de VRAM reservada, que a tabela de residencia mostra.
//
// Standalone de proposito — nao linka a SmileEngine. Cria o proprio device.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <D3D12MA/D3D12MemAlloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

    // AllocateMemory exige SizeInBytes multiplo de 64 KB (documentado no header da lib).
    constexpr UINT64 kHeapGranularity = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64 KB

    bool Check(HRESULT Hr, const char* What) {
        if (SUCCEEDED(Hr)) return true;
        std::printf("FALHOU %s (hr=0x%08lX)\n", What, static_cast<unsigned long>(Hr));
        return false;
    }

    double MsSince(Clock::time_point Start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
    }

    D3D12_RESOURCE_DESC Tex2D(UINT64 W, UINT H, DXGI_FORMAT Fmt, D3D12_RESOURCE_FLAGS Flags) {
        D3D12_RESOURCE_DESC D{};
        D.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        D.Width            = W;
        D.Height           = H;
        D.DepthOrArraySize = 1;
        D.MipLevels        = 1;
        D.Format           = Fmt;
        D.SampleDesc       = { 1, 0 };
        D.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D.Flags            = Flags;
        return D;
    }

    // Le o arquivo que a engine escreve com SMILE_CAPTURE_DESCS. Este e o caminho PREFERIDO:
    // sao os descritores reais do RecreateInternalTargets, e nao uma aproximacao. So os do
    // heap DEFAULT entram — o benchmark compara committed contra placed, e UPLOAD/READBACK
    // tem custo por MB bem diferente, entao misturar as duas populacoes envenenaria
    // qualquer coeficiente extraido daqui.
    std::vector<D3D12_RESOURCE_DESC> LoadCapturedSet(const char* Path, size_t& OutSkipped) {
        std::vector<D3D12_RESOURCE_DESC> Out;
        OutSkipped = 0;

        std::ifstream File(Path);
        if (!File) return Out;

        std::string Line;
        bool SawV2 = false;
        while (std::getline(File, Line)) {
            if (Line.rfind("# smile desc capture v2", 0) == 0) { SawV2 = true; continue; }
            if (Line.empty() || Line[0] == '#') continue;

            // Recusa formato antigo em vez de interpretar campos na posicao errada: um
            // arquivo v1 lido como v2 daria descritores silenciosamente errados, que e
            // exatamente a classe de erro que este benchmark existe para nao cometer.
            if (!SawV2) { Out.clear(); return Out; }

            std::istringstream S(Line);
            int Dim = 0, Fmt = 0, Layout = 0, Flags = 0, HeapType = 0;
            unsigned long long W = 0, Alignment = 0;
            unsigned Ht = 0, Arr = 0, Mips = 0, SampleCount = 0, SampleQuality = 0;
            if (!(S >> Dim >> Alignment >> W >> Ht >> Arr >> Mips >> Fmt
                    >> SampleCount >> SampleQuality >> Layout >> Flags >> HeapType))
                continue;
            if (HeapType != D3D12_HEAP_TYPE_DEFAULT) { ++OutSkipped; continue; }

            D3D12_RESOURCE_DESC D{};
            D.Dimension        = static_cast<D3D12_RESOURCE_DIMENSION>(Dim);
            D.Alignment        = Alignment;
            D.Width            = W;
            D.Height           = Ht;
            D.DepthOrArraySize = static_cast<UINT16>(Arr);
            D.MipLevels        = static_cast<UINT16>(Mips);
            D.Format           = static_cast<DXGI_FORMAT>(Fmt);
            D.SampleDesc       = { SampleCount, SampleQuality };
            D.Layout           = static_cast<D3D12_TEXTURE_LAYOUT>(Layout);
            D.Flags            = static_cast<D3D12_RESOURCE_FLAGS>(Flags);
            Out.push_back(D);
        }
        return Out;
    }

    // Conjunto APROXIMADO. So roda com --approx explicito, e essa exigencia foi comprada com
    // dois erros: a v1 do modelo de custo saiu daqui (somava 787 MB contra 605 MB reais) e a
    // primeira leitura do D3D12MA tambem (deu -33% onde a captura real de 330,8 MB da -53,7%,
    // e o numero foi comparado com o do load como se fossem a mesma populacao).
    //
    // Nas duas vezes o aviso impresso estava la e foi ignorado — na segunda, por quem o
    // escreveu. Um caminho de medicao com esse historico nao pode ser o default: quem quiser
    // a aproximacao pede por ela, e quem so rodar o binario cai no caminho que nao mente.
    std::vector<D3D12_RESOURCE_DESC> BuildResizeSet(UINT W, UINT H) {
        constexpr auto kUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr auto kRt  = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        std::vector<D3D12_RESOURCE_DESC> Out;
        auto Add = [&](int N, DXGI_FORMAT Fmt, D3D12_RESOURCE_FLAGS Flags, int Div = 1) {
            for (int i = 0; i < N; ++i)
                Out.push_back(Tex2D(std::max(1u, W / Div), std::max(1u, H / Div), Fmt, Flags));
        };

        Add(3,  DXGI_FORMAT_R8G8B8A8_UNORM,       kRt);        // G-buffer
        Add(6,  DXGI_FORMAT_R16G16B16A16_FLOAT,   kRt);        // HDR, copias, TAA, display
        Add(2,  DXGI_FORMAT_R32_TYPELESS,         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        Add(2,  DXGI_FORMAT_R16G16_FLOAT,         kRt);        // velocity + masks
        Add(8,  DXGI_FORMAT_R32G32B32A32_FLOAT,   kUav);       // reservoirs ReSTIR (x1)
        Add(8,  DXGI_FORMAT_R32G32B32A32_UINT,    kUav);       // reservoirs ReSTIR (x2)
        Add(12, DXGI_FORMAT_R16G16B16A16_FLOAT,   kUav);       // GI, reflexoes, NRD in/out
        Add(6,  DXGI_FORMAT_R16G16B16A16_FLOAT,   kUav);       // guides do DLSS-RR
        Add(6,  DXGI_FORMAT_R8_UNORM,             kUav);       // AO e mascaras
        Add(9,  DXGI_FORMAT_R16G16B16A16_FLOAT,   kRt, 4);     // cadeia de bloom
        Add(3,  DXGI_FORMAT_R16G16B16A16_FLOAT,   kRt, 2);     // sun shafts
        Add(3,  DXGI_FORMAT_R16G16B16A16_FLOAT,   kUav, 2);    // nuvens + historico
        Add(1,  DXGI_FORMAT_R32_FLOAT,            kUav);       // HZB
        Add(10, DXGI_FORMAT_R16G16B16A16_FLOAT,   kUav);       // resto dos intermediarios
        return Out;
    }

    struct FRun {
        double Ms            = 0.0;
        UINT64 Bytes         = 0; // soma dos GetResourceAllocationInfo do conjunto
        UINT64 BlockBytes    = 0; // so D3D12MA: heap do pool no PICO (tudo vivo)
        UINT64 RetainedBytes = 0; // so D3D12MA: heap que sobra APOS liberar tudo
        bool   Ok            = false;
    };

    UINT64 SumBytes(ID3D12Device* Dev, const std::vector<D3D12_RESOURCE_DESC>& Descs) {
        UINT64 Total = 0;
        for (const auto& D : Descs) Total += Dev->GetResourceAllocationInfo(0, 1, &D).SizeInBytes;
        return Total;
    }

    FRun RunCommitted(ID3D12Device* Dev, const std::vector<D3D12_RESOURCE_DESC>& Descs,
                      D3D12_HEAP_FLAGS HeapFlags) {
        std::vector<ComPtr<ID3D12Resource>> Keep(Descs.size());
        D3D12_HEAP_PROPERTIES Hp{}; Hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        FRun R;
        const auto Start = Clock::now();
        for (size_t i = 0; i < Descs.size(); ++i) {
            if (FAILED(Dev->CreateCommittedResource(&Hp, HeapFlags, &Descs[i],
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&Keep[i]))))
                return R;
        }
        R.Ms    = MsSince(Start);
        R.Bytes = SumBytes(Dev, Descs);
        R.Ok    = true;
        return R;
    }

    // Offset de placed tem que respeitar o Alignment que o device pede para cada recurso.
    UINT64 ComputePlacedLayout(ID3D12Device* Dev, const std::vector<D3D12_RESOURCE_DESC>& Descs,
                               std::vector<UINT64>& OutOffsets) {
        OutOffsets.resize(Descs.size());
        UINT64 Total = 0;
        for (size_t i = 0; i < Descs.size(); ++i) {
            const auto Info = Dev->GetResourceAllocationInfo(0, 1, &Descs[i]);
            Total          = (Total + Info.Alignment - 1) & ~(Info.Alignment - 1);
            OutOffsets[i]  = Total;
            Total         += Info.SizeInBytes;
        }
        return Total;
    }

    ComPtr<ID3D12Heap> MakePlacedHeap(ID3D12Device* Dev, UINT64 Bytes) {
        D3D12_HEAP_DESC Hd{};
        Hd.SizeInBytes     = Bytes;
        Hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        Hd.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        // Tier 2: um heap serve buffer, textura comum e RT/DS. Em Tier 1 isto falha, e e por
        // isso que o main checa o tier antes de chamar aqui.
        Hd.Flags           = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

        ComPtr<ID3D12Heap> Heap;
        if (!Check(Dev->CreateHeap(&Hd, IID_PPV_ARGS(&Heap)), "CreateHeap")) return {};
        return Heap;
    }

    // HeapInside = o heap entra no cronometro. Fora dele, a medida isola o custo do OBJETO
    // de recurso, que e o piso que nenhum allocator remove.
    //
    // Reuse != null e o heap PERSISTENTE do main, e nao um detalhe: com um heap novo a cada
    // chamada, este caminho oscilou entre 1,65 e 12,34 ms em execucoes seguidas, porque o
    // custo saia do cronometro mas a pressao de pagina que ele deixava, nao. Um teto que
    // varia 7x nao serve de teto — e chegou a ficar ACIMA do pool retido, imprimindo "112,9%
    // da economia possivel". Reusar o heap e o que torna esta linha comparavel ao pool
    // retido, que tambem retem memoria: e a mesma situacao medida dos dois lados.
    FRun RunPlaced(ID3D12Device* Dev, const std::vector<D3D12_RESOURCE_DESC>& Descs,
                   bool HeapInside, ID3D12Heap* Reuse) {
        std::vector<UINT64> Offsets;
        const UINT64 Total = ComputePlacedLayout(Dev, Descs, Offsets);

        FRun R;
        R.Bytes = Total;

        ComPtr<ID3D12Heap> Owned;
        if (!HeapInside && !Reuse) {
            Owned = MakePlacedHeap(Dev, Total);
            if (!Owned) return R;
        }
        ID3D12Heap* Heap = Reuse ? Reuse : Owned.Get();

        std::vector<ComPtr<ID3D12Resource>> Keep(Descs.size());
        const auto Start = Clock::now();
        if (HeapInside) {
            Owned = MakePlacedHeap(Dev, Total);
            if (!Owned) return R;
            Heap = Owned.Get();
        }
        for (size_t i = 0; i < Descs.size(); ++i) {
            if (FAILED(Dev->CreatePlacedResource(Heap, Offsets[i], &Descs[i],
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&Keep[i]))))
                return R;
        }
        R.Ms = MsSince(Start);
        R.Ok = true;
        return R;
    }

    // === D3D12MA =================================================================

    ComPtr<D3D12MA::Allocator> MakeAllocator(ID3D12Device* Dev, IDXGIAdapter* Adapter) {
        D3D12MA::ALLOCATOR_DESC Desc{};
        // Os flags RECOMENDADOS pela lib, que e como ela seria usada de verdade. Incluem
        // DEFAULT_POOLS_NOT_ZEROED — por isso o caminho (2) existe: sem ele nao da para
        // separar "sub-alocou" de "nao zerou pagina".
        Desc.Flags    = D3D12MA_RECOMMENDED_ALLOCATOR_FLAGS;
        Desc.pDevice  = Dev;
        Desc.pAdapter = Adapter;

        ComPtr<D3D12MA::Allocator> Out;
        if (!Check(D3D12MA::CreateAllocator(&Desc, &Out), "D3D12MA::CreateAllocator"))
            return {};
        return Out;
    }

    // Pool com memoria RETIDA. MinBlockCount mantem os blocos vivos "even if they stay empty",
    // que e a unica forma de o caso quente ser quente de verdade: sem isso a lib devolve o
    // heap assim que o bloco esvazia, e a passada seguinte paga o commit de pagina de novo.
    // E o trade-off explicito de um pool — VRAM reservada em troca de tempo — em vez de uma
    // esperanca sobre a politica interna da lib.
    constexpr UINT64 kRetainedBlockSize = 256ull << 20;

    UINT RetainedBlockCount(UINT64 TotalBytes) {
        return static_cast<UINT>(
            (TotalBytes * 5 / 4 + kRetainedBlockSize - 1) / kRetainedBlockSize);
    }

    UINT64 RetainedPoolBytes(UINT64 TotalBytes) {
        return static_cast<UINT64>(RetainedBlockCount(TotalBytes)) * kRetainedBlockSize;
    }

    ComPtr<D3D12MA::Pool> MakeRetainedPool(D3D12MA::Allocator* Alloc, UINT64 TotalBytes) {

        D3D12MA::POOL_DESC Desc{};
        Desc.Flags               = D3D12MA_RECOMMENDED_POOL_FLAGS;
        Desc.HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        // 0 = ALLOW_ALL_BUFFERS_AND_TEXTURES em Tier 2. O CREATE_NOT_ZEROED acompanha o que os
        // flags recomendados do allocator ja fazem, senao a comparacao entre os dois caminhos
        // do D3D12MA mediria tambem a diferenca de zeramento.
        Desc.HeapFlags           = D3D12MA_RECOMMENDED_HEAP_FLAGS;
        Desc.BlockSize           = kRetainedBlockSize;
        // Folga de 25% para fragmentacao: o conjunto nao empacota perfeito em blocos fixos, e
        // um bloco a mais alocado sob demanda tiraria justamente o efeito que se quer medir.
        Desc.MinBlockCount = RetainedBlockCount(TotalBytes);
        Desc.MaxBlockCount = 0; // sem teto: estourar aqui viraria falha de alocacao, nao medida

        ComPtr<D3D12MA::Pool> Out;
        if (!Check(Alloc->CreatePool(&Desc, &Out), "D3D12MA::CreatePool")) return {};
        return Out;
    }

    enum class EMaMode { CreateResource, AllocateAndAlias };

    FRun RunMa(ID3D12Device* Dev, D3D12MA::Allocator* Alloc,
               const std::vector<D3D12_RESOURCE_DESC>& Descs, EMaMode Mode,
               D3D12MA::Pool* Pool = nullptr) {
        FRun R;
        if (!Alloc) return R;

        std::vector<ComPtr<ID3D12Resource>>      Keep(Descs.size());
        std::vector<ComPtr<D3D12MA::Allocation>> Allocs(Descs.size());

        D3D12MA::ALLOCATION_DESC Ad{};
        Ad.HeapType   = D3D12_HEAP_TYPE_DEFAULT;
        Ad.CustomPool = Pool; // quando != null, HeapType e ExtraHeapFlags sao ignorados
        // ExtraHeapFlags fica em 0 DE PROPOSITO. A doc do ALLOCATION_DESC e explicita: com
        // qualquer flag extra a lib passa a alocar bloco separado por recurso, que e
        // exatamente a sub-alocacao que se quer medir indo embora. Em Tier 2 o 0 ja vale como
        // ALLOW_ALL_BUFFERS_AND_TEXTURES; em Tier 1 o main aborta antes de chegar aqui.

        const auto Start = Clock::now();
        for (size_t i = 0; i < Descs.size(); ++i) {
            if (Mode == EMaMode::CreateResource) {
                if (FAILED(Alloc->CreateResource(&Ad, &Descs[i], D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, &Allocs[i], IID_PPV_ARGS(&Keep[i]))))
                    return R;
            } else {
                // O caminho que a FACHADA usaria. Aqui a Allocation e memoria crua e NAO
                // conhece o recurso ("The resource is not bound with pAllocation", diz a doc
                // do CreateAliasingResource), entao nao ha ciclo de referencia e o
                // destruction notifier do recurso pode ser o dono da liberacao — o que mantem
                // ComPtr<ID3D12Resource> como tipo de retorno da GpuResources e poupa ~50
                // call sites. CreateResource nao serve para isso: la a Allocation "owns a
                // reference" ao recurso, o refcount nunca chega a zero e o notifier nunca
                // dispara. Ver CheckNotifierDesign, que prova as duas coisas.
                //
                // O GetResourceAllocationInfo fica DENTRO do cronometro porque e custo real
                // deste caminho; o CreateResource paga o equivalente por dentro. O caminho
                // placed calcula o layout fora, e por isso e teto e nao concorrente justo.
                D3D12_RESOURCE_ALLOCATION_INFO Info =
                    Dev->GetResourceAllocationInfo(0, 1, &Descs[i]);
                Info.SizeInBytes =
                    (Info.SizeInBytes + kHeapGranularity - 1) & ~(kHeapGranularity - 1);

                if (FAILED(Alloc->AllocateMemory(&Ad, &Info, &Allocs[i]))) return R;
                if (FAILED(Alloc->CreateAliasingResource(Allocs[i].Get(), 0, &Descs[i],
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&Keep[i]))))
                    return R;
            }
        }
        R.Ms = MsSince(Start);

        // Fora do cronometro. Duas medidas de VRAM, e a segunda e a que decide:
        //   PICO    = heap do pool com todos os recursos vivos.
        //   RETIDO  = heap que sobra DEPOIS de liberar tudo. Se cair a zero, a lib devolveu os
        //             blocos ao SO e a proxima passada vai pagar o commit de pagina de novo —
        //             ou seja, "quente" nao era quente. Sem esta linha o benchmark reportaria
        //             o pico e deixaria a conclusao errada de pe.
        auto BlockBytesNow = [&] {
            D3D12MA::Budget B{};
            Alloc->GetBudget(&B, nullptr);
            return B.Stats.BlockBytes;
        };
        R.BlockBytes = BlockBytesNow();
        Keep.clear();
        Allocs.clear();
        R.RetainedBytes = BlockBytesNow();

        R.Bytes = SumBytes(Dev, Descs);
        R.Ok    = true;
        return R;
    }

    // === Validacao do design da fachada ==========================================
    // A integracao na engine depende de UMA hipotese: dá para liberar a Allocation no
    // destruction callback do recurso, e assim manter ComPtr<ID3D12Resource> como tipo de
    // retorno da GpuResources. Se a hipotese for falsa, a integracao passa a exigir mudar o
    // tipo de retorno em ~50 call sites — outra conta, outra decisao. Melhor descobrir aqui,
    // em 30 linhas, do que no meio do refactor.

    struct FNotifierProbe {
        D3D12MA::Allocation* Alloc = nullptr; // ref CRUA de proposito: o callback e o dono
        bool                 Fired = false;
    };

    void CALLBACK OnProbeDestroyed(void* Data) {
        auto* P = static_cast<FNotifierProbe*>(Data);
        P->Fired = true;
        if (P->Alloc) { P->Alloc->Release(); P->Alloc = nullptr; }
    }

    bool CheckNotifierDesign(ID3D12Device* Dev, IDXGIAdapter* Adapter) {
        ComPtr<D3D12MA::Allocator> Alloc = MakeAllocator(Dev, Adapter);
        if (!Alloc) return false;

        D3D12MA::ALLOCATION_DESC Ad{};
        Ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Buf{};
        Buf.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Buf.Width            = kHeapGranularity;
        Buf.Height           = 1;
        Buf.DepthOrArraySize = 1;
        Buf.MipLevels        = 1;
        Buf.Format           = DXGI_FORMAT_UNKNOWN;
        Buf.SampleDesc       = { 1, 0 };
        Buf.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        auto UsedBytes = [&] {
            D3D12MA::Budget B{};
            Alloc->GetBudget(&B, nullptr);
            return B.Stats.AllocationBytes;
        };
        const UINT64 Before = UsedBytes();

        // --- Hipotese A: AllocateMemory + CreateAliasingResource ---------------------
        FNotifierProbe ProbeA;
        {
            D3D12_RESOURCE_ALLOCATION_INFO Info = Dev->GetResourceAllocationInfo(0, 1, &Buf);
            Info.SizeInBytes =
                (Info.SizeInBytes + kHeapGranularity - 1) & ~(kHeapGranularity - 1);

            if (!Check(Alloc->AllocateMemory(&Ad, &Info, &ProbeA.Alloc), "AllocateMemory"))
                return false;

            ComPtr<ID3D12Resource> Res;
            if (!Check(Alloc->CreateAliasingResource(ProbeA.Alloc, 0, &Buf,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&Res)),
                       "CreateAliasingResource")) {
                ProbeA.Alloc->Release();
                return false;
            }

            ComPtr<ID3DDestructionNotifier> Notifier;
            if (FAILED(Res->QueryInterface(IID_PPV_ARGS(&Notifier)))) {
                std::printf("  ID3DDestructionNotifier indisponivel nesta maquina.\n");
                ProbeA.Alloc->Release();
                return false;
            }
            UINT Cookie = 0;
            if (!Check(Notifier->RegisterDestructionCallback(
                           &OnProbeDestroyed, &ProbeA, &Cookie),
                       "RegisterDestructionCallback (alias)")) {
                ProbeA.Alloc->Release();
                ProbeA.Alloc = nullptr;
                return false;
            }
        } // Res morre aqui: e o unico dono do recurso, entao o callback tem que disparar.

        const bool OkA = ProbeA.Fired && UsedBytes() == Before;
        std::printf("  AllocateMemory + CreateAliasingResource: callback %s, memoria %s\n",
                    ProbeA.Fired ? "disparou" : "NAO DISPAROU",
                    UsedBytes() == Before ? "devolvida ao pool" : "RETIDA");

        // --- Hipotese B: CreateResource (deve TRAVAR, e e por isso que nao serve) -----
        // Provar em vez de citar a doc. Sem vazar: no fim libera-se a Allocation na mao, o
        // que solta a ref que ela mantem no recurso e so entao o callback dispara.
        FNotifierProbe ProbeB;
        bool FiredWhileResourcePtrGone = false;
        {
            ComPtr<ID3D12Resource> Res;
            if (!Check(Alloc->CreateResource(&Ad, &Buf, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             &ProbeB.Alloc, IID_PPV_ARGS(&Res)),
                       "CreateResource"))
                return false;

            ComPtr<ID3DDestructionNotifier> Notifier;
            if (!Check(Res->QueryInterface(IID_PPV_ARGS(&Notifier)),
                       "QueryInterface ID3DDestructionNotifier (CreateResource)")) {
                D3D12MA::Allocation* Owned = std::exchange(ProbeB.Alloc, nullptr);
                if (Owned) Owned->Release();
                return false;
            }
            UINT Cookie = 0;
            if (!Check(Notifier->RegisterDestructionCallback(
                           &OnProbeDestroyed, &ProbeB, &Cookie),
                       "RegisterDestructionCallback (CreateResource)")) {
                D3D12MA::Allocation* Owned = std::exchange(ProbeB.Alloc, nullptr);
                if (Owned) Owned->Release();
                return false;
            }

            // A interface de notifier tambem e um ComPtr para o MESMO objeto D3D12. Ela tem
            // que morrer antes do Res, senao o teste concluiria "callback nao disparou" so
            // porque ele proprio ainda mantinha uma referencia viva.
            Notifier.Reset();
            Res.Reset();
            FiredWhileResourcePtrGone = ProbeB.Fired;
        }
        // Zera ANTES do Release: liberar a Allocation solta a referencia que ela guarda no
        // recurso e dispara o callback de forma reentrante. O callback nao pode dar Release
        // de novo na Allocation que ja esta em destruicao.
        D3D12MA::Allocation* OwnedB = std::exchange(ProbeB.Alloc, nullptr);
        if (OwnedB) OwnedB->Release();

        std::printf("  CreateResource: apos soltar o ComPtr do recurso, callback %s\n",
                    FiredWhileResourcePtrGone
                        ? "disparou (a doc estaria errada — reveja o design)"
                        : "NAO disparou (a Allocation segura o recurso, como a doc diz)");

        return OkA && !FiredWhileResourcePtrGone;
    }

    double Median(std::vector<double> V) {
        if (V.empty()) return 0.0;
        std::sort(V.begin(), V.end());
        return V[V.size() / 2];
    }

    struct FCase {
        const char*           Name;
        std::function<FRun()> Run;
        std::vector<double>   Samples;
        UINT64                Bytes         = 0;
        UINT64                BlockBytes    = 0;
        UINT64                RetainedBytes = 0;
        bool                  IsPool        = false;
    };

    void Report(const FCase& C, size_t N, double BaselineMs) {
        if (C.Samples.empty()) { std::printf("  %-30s FALHOU\n", C.Name); return; }
        const double Med = Median(C.Samples);
        std::printf("  %-30s %7.2f ms   %6.3f ms/rec   %7.1f MB   [%.1f-%.1f]",
                    C.Name, Med, Med / static_cast<double>(N),
                    static_cast<double>(C.Bytes) / (1024.0 * 1024.0),
                    *std::min_element(C.Samples.begin(), C.Samples.end()),
                    *std::max_element(C.Samples.begin(), C.Samples.end()));
        if (BaselineMs > 0.0)
            std::printf("   %+6.1f%%", (Med / BaselineMs - 1.0) * 100.0);
        std::printf("\n");
    }

} // namespace

int main(int argc, char** argv) {
    // Uso:
    //   SmileAllocBench <captura.txt>                 <- descritores REAIS da engine
    //   SmileAllocBench --approx [largura] [altura]   <- conjunto aproximado (ver o comentario
    //                                                    do BuildResizeSet antes de usar)
    const char* CapturePath = nullptr;
    bool Approx = false;
    UINT W = 1573, H = 804;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--approx") == 0) { Approx = true; continue; }
        if (!Approx && !CapturePath) { CapturePath = argv[i]; continue; }
        if (Approx && i + 1 < argc) { W = std::atoi(argv[i]); H = std::atoi(argv[i + 1]); ++i; }
    }

    if (!CapturePath && !Approx) {
        std::printf(
            "Uso: SmileAllocBench <captura.txt>\n"
            "     SmileAllocBench --approx [largura] [altura]\n\n"
            "Sem argumento este benchmark nao roda, e isso e proposital. O conjunto\n"
            "aproximado ja produziu duas conclusoes erradas nesta frente por ser o default:\n"
            "os coeficientes por MB tirados dele nao valem, e comparar o resultado dele com\n"
            "o de uma captura real compara populacoes diferentes.\n\n"
            "Para gerar a captura, rode o editor em build de diagnostico\n"
            "(build/bin/RelWithDebInfo/SmileEditor.exe):\n"
            "  - carregar uma cena escreve smile-load-descs.txt\n"
            "  - redimensionar a janela escreve smile-resize-descs.txt\n"
            "O log do editor imprime o caminho absoluto dos dois.\n");
        return 1;
    }

    constexpr int kIterations = 5;

    ComPtr<IDXGIFactory6> Factory;
    if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory)), "CreateDXGIFactory2")) return 1;

    ComPtr<IDXGIAdapter1> Adapter;
    if (!Check(Factory->EnumAdapterByGpuPreference(
                   0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&Adapter)),
               "EnumAdapterByGpuPreference"))
        return 1;

    ComPtr<ID3D12Device> Device;
    if (!Check(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)),
               "D3D12CreateDevice"))
        return 1;

    DXGI_ADAPTER_DESC1 Ad{};
    Adapter->GetDesc1(&Ad);
    std::wprintf(L"Adaptador: %s\n", Ad.Description);

    D3D12_FEATURE_DATA_D3D12_OPTIONS Options{};
    Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &Options, sizeof(Options));
    std::printf("Resource Heap Tier: %d\n", static_cast<int>(Options.ResourceHeapTier));

    // Gate DE VERDADE. O caminho placed usa UM heap com ALLOW_ALL_BUFFERS_AND_TEXTURES, que
    // Tier 1 nao aceita: la seria preciso um heap por classe de recurso. Antes isto era so um
    // comentario prometendo uma checagem que nao existia, e em Tier 1 o benchmark falharia
    // com um HRESULT solto em vez de dizer o que houve.
    if (Options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2) {
        std::printf("\nEste benchmark exige Resource Heap Tier 2 (heap unico misturando\n"
                    "buffer, textura e RT/DS). Em Tier 1 seria preciso um heap por classe,\n"
                    "e o numero deixaria de ser comparavel. Abortando.\n");
        return 2;
    }

    size_t Skipped = 0;
    std::vector<D3D12_RESOURCE_DESC> Descs;
    if (CapturePath) {
        Descs = LoadCapturedSet(CapturePath, Skipped);
        if (Descs.empty()) {
            std::printf("\nNao consegui ler descritores de '%s'.\n", CapturePath);
            return 1;
        }
        std::printf("\nConjunto CAPTURADO de '%s': %zu recursos DEFAULT"
                    " (%zu UPLOAD/READBACK ignorados)\n", CapturePath, Descs.size(), Skipped);
    } else {
        Descs = BuildResizeSet(W, H);
        std::printf("\nConjunto APROXIMADO (%ux%u): %zu recursos.\n"
                    "  AVISO: nao sao os descritores da engine. Serve para a comparacao entre\n"
                    "  caminhos, NAO para extrair coeficiente por MB. Rode a engine com\n"
                    "  SMILE_CAPTURE_DESCS=<arquivo>, faca um resize, e passe o arquivo aqui.\n",
                    W, H, Descs.size());
    }

    // Orcamento. Cada caso isola e destroi seu fixture, entao o pico e o maior caso individual:
    // o pool retido arredondado em blocos de 256 MB. Se nem ele couber, o driver comeca a
    // paginar e o numero deixa de ser tempo de criacao sem nada na saida denunciar a troca.
    {
        ComPtr<IDXGIAdapter3> Adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO Mem{};
        const UINT64 SetBytes = SumBytes(Device.Get(), Descs);
        if (SUCCEEDED(Adapter.As(&Adapter3)) &&
            SUCCEEDED(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Mem))) {
            // Cada caso agora cria e destroi o proprio fixture. O maior e o pool retido,
            // arredondado em blocos de 256 MB; ele NAO coexiste com o heap do teto nem com os
            // allocators dos outros casos.
            const double NeedMB  = static_cast<double>(RetainedPoolBytes(SetBytes)) /
                                   (1024.0 * 1024.0);
            const double FreeMB  =
                static_cast<double>(Mem.Budget - Mem.CurrentUsage) / (1024.0 * 1024.0);
            const double TotalMB = static_cast<double>(Ad.DedicatedVideoMemory) / (1024.0 * 1024.0);
            const double BudgetMB = static_cast<double>(Mem.Budget) / (1024.0 * 1024.0);

            std::printf("VRAM: orcamento de %.0f MB numa placa de %.0f MB; %.0f MB livres."
                        " O benchmark pede ~%.0f MB no pico.\n",
                        BudgetMB, TotalMB, FreeMB, NeedMB);

            // O orcamento e por PROCESSO e o Windows o encolhe quando outro processo disputa a
            // placa. Um orcamento bem abaixo da VRAM fisica e a assinatura de que ha algo
            // pesado aberto — e sem registrar isso, um numero medido nessas condicoes fica
            // orfao do contexto: quem ler o log depois nao tem como saber por que a dispersao
            // foi de 3x. Foi o que aconteceu na primeira rodada, medida com um jogo aberto.
            if (BudgetMB < TotalMB * 0.75)
                std::printf("  AVISO: o orcamento esta em %.0f%% da VRAM fisica — provavelmente\n"
                            "  ha outro processo pesado usando a GPU. As RAZOES entre caminhos\n"
                            "  resistem (mediana + ordem rotacionada), os valores ABSOLUTOS nao.\n",
                            BudgetMB / TotalMB * 100.0);
            if (NeedMB > FreeMB)
                std::printf("  AVISO: nao cabe. O que sair daqui pode ser tempo de PAGINACAO e\n"
                            "  nao de criacao. Feche o editor, ou meca um conjunto menor.\n");
        }
    }

    std::printf("\n=== Design da fachada: liberar a Allocation no destruction callback ===\n");
    const bool NotifierOk = CheckNotifierDesign(Device.Get(), Adapter.Get());
    std::printf("  => %s\n", NotifierOk
        ? "VALIDO: da para plugar o D3D12MA sem mudar o tipo de retorno da GpuResources."
        : "INVALIDO: a integracao exigiria carregar a Allocation junto do recurso.");

    const UINT64 SetBytes = SumBytes(Device.Get(), Descs);
    std::vector<FCase> Cases;
    Cases.push_back({ "committed", [&] {
        return RunCommitted(Device.Get(), Descs, D3D12_HEAP_FLAG_NONE); } });
    Cases.push_back({ "committed + NOT_ZEROED", [&] {
        return RunCommitted(Device.Get(), Descs, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED); } });
    Cases.push_back({ "placed (heap pronto)", [&] {
        std::vector<UINT64> Offsets;
        ComPtr<ID3D12Heap> Heap =
            MakePlacedHeap(Device.Get(), ComputePlacedLayout(Device.Get(), Descs, Offsets));
        return RunPlaced(Device.Get(), Descs, false, Heap.Get()); } });
    Cases.push_back({ "placed (heap dentro)", [&] {
        return RunPlaced(Device.Get(), Descs, true, nullptr); } });
    Cases.push_back({ "D3D12MA frio (CreateRes)", [&] {
        ComPtr<D3D12MA::Allocator> A = MakeAllocator(Device.Get(), Adapter.Get());
        return RunMa(Device.Get(), A.Get(), Descs, EMaMode::CreateResource); } });
    Cases.push_back({ "D3D12MA quente (CreateRes)", [&] {
        ComPtr<D3D12MA::Allocator> A = MakeAllocator(Device.Get(), Adapter.Get());
        if (!A) return FRun{};
        (void)RunMa(Device.Get(), A.Get(), Descs, EMaMode::CreateResource);
        return RunMa(Device.Get(), A.Get(), Descs, EMaMode::CreateResource); } });
    Cases.push_back({ "D3D12MA frio (alias)", [&] {
        ComPtr<D3D12MA::Allocator> A = MakeAllocator(Device.Get(), Adapter.Get());
        return RunMa(Device.Get(), A.Get(), Descs, EMaMode::AllocateAndAlias); } });
    Cases.push_back({ "D3D12MA quente (alias)", [&] {
        ComPtr<D3D12MA::Allocator> A = MakeAllocator(Device.Get(), Adapter.Get());
        if (!A) return FRun{};
        (void)RunMa(Device.Get(), A.Get(), Descs, EMaMode::AllocateAndAlias);
        return RunMa(Device.Get(), A.Get(), Descs, EMaMode::AllocateAndAlias); } });
    Cases.push_back({ "D3D12MA pool retido (alias)", [&] {
        ComPtr<D3D12MA::Allocator> A = MakeAllocator(Device.Get(), Adapter.Get());
        if (!A) return FRun{};
        ComPtr<D3D12MA::Pool> Pool = MakeRetainedPool(A.Get(), SetBytes);
        if (!Pool) return FRun{};
        return RunMa(Device.Get(), A.Get(), Descs, EMaMode::AllocateAndAlias,
                     Pool.Get()); } });
    for (size_t i = 4; i < Cases.size(); ++i) Cases[i].IsPool = true;

    // Uma passada descartada por caminho: a primeira alocacao de VRAM do processo paga
    // inicializacao do driver que nao se repete, e ela cairia inteira no primeiro caso
    // medido. Nos casos QUENTES esta passada e tambem o que aquece o pool — sem ela a
    // primeira iteracao deles seria fria e a mediana misturaria as duas situacoes.
    for (FCase& C : Cases) (void)C.Run();

    // Ordem ROTACIONADA entre iteracoes. Com ordem fixa, o caminho medido primeiro paga o
    // estado de pagina deixado pelo anterior sempre do mesmo jeito, e a razao entre eles
    // herda esse vies — foi o que fez as faixas relatadas (-97/-98%) nao baterem com uma
    // execucao independente (-99,1%).
    for (int It = 0; It < kIterations; ++It) {
        for (size_t k = 0; k < Cases.size(); ++k) {
            FCase& C = Cases[(k + static_cast<size_t>(It)) % Cases.size()];
            const FRun R = C.Run();
            if (!R.Ok) continue;
            C.Samples.push_back(R.Ms);
            C.Bytes         = R.Bytes;
            C.BlockBytes    = R.BlockBytes;
            C.RetainedBytes = R.RetainedBytes;
        }
    }

    std::printf("\n%-32s %10s %14s %11s %14s %8s\n",
                "", "mediana", "por recurso", "bytes", "[min-max]", "vs comm");
    const double Base = Median(Cases[0].Samples);
    for (const FCase& C : Cases) Report(C, Descs.size(), &C == &Cases[0] ? 0.0 : Base);

    // Residencia. Duas colunas porque elas contam historias opostas: o pico e quanto custa em
    // VRAM ter o pool, e o retido e o que sobra para a proxima passada nao repagar o commit
    // de pagina. Retido = 0 significa que a lib devolveu tudo e que "quente" era so um nome.
    std::printf("\nVRAM do pool (pico com tudo vivo x retido apos liberar):\n");
    for (const FCase& C : Cases) {
        if (!C.IsPool || C.Samples.empty()) continue;
        std::printf("  %-30s %7.1f MB de pico   %7.1f MB retidos\n", C.Name,
                    static_cast<double>(C.BlockBytes)    / (1024.0 * 1024.0),
                    static_cast<double>(C.RetainedBytes) / (1024.0 * 1024.0));
    }

    if (!Cases[0].Samples.empty() && !Cases[2].Samples.empty()) {
        const double Ready = Median(Cases[2].Samples);
        std::printf(
            "\nMedianas de %d iteracoes em ordem rotacionada.\n"
            "TETO de economia de um sub-alocador: %.3f ms/recurso (%.1f%% do custo atual).\n"
            "Piso restante (%.3f ms/recurso) e o objeto de recurso, que placed NAO remove.\n",
            kIterations, (Base - Ready) / static_cast<double>(Descs.size()),
            (Base - Ready) / Base * 100.0, Ready / static_cast<double>(Descs.size()));
    }

    // A fracao do teto que a lib entrega — a pergunta que este benchmark existe para
    // responder. Pelo MELHOR caminho da lib e nao por um indice fixo: qual deles ganha e
    // justamente o que nao se sabia ao escrever isto.
    const FCase* Best = nullptr;
    for (const FCase& C : Cases)
        if (C.IsPool && !C.Samples.empty() &&
            (!Best || Median(C.Samples) < Median(Best->Samples)))
            Best = &C;

    if (Best && !Cases[0].Samples.empty() && !Cases[2].Samples.empty()) {
        const double Ready = Median(Cases[2].Samples);
        const double Lib   = Median(Best->Samples);
        std::printf("Melhor caminho da lib (%s) entrega %.1f%% da economia possivel"
                    " (teto = %.2f ms, lib = %.2f ms, base = %.2f ms).\n",
                    Best->Name, (Base - Lib) / (Base - Ready) * 100.0, Ready, Lib, Base);
    }
    return 0;
}
