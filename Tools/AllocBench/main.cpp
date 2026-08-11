// SmileAllocBench — quanto do custo de criar recurso e o HEAP IMPLICITO?
//
// A instrumentacao da engine mediu, na Bistro, um custo de ~0,374 ms fixo por recurso mais
// ~0,022 ms/MB. O termo fixo justificaria trocar committed por placed — CreatePlacedResource
// nao cria heap — MAS o intercepto contem duas coisas: criar o heap E criar o objeto de
// recurso. Placed so remove a primeira. Sem separar as duas, "placed economiza 176 ms no
// load" e extrapolacao, nao medida.
//
// Este benchmark separa. Mesmos descritores, tres caminhos:
//
//   1. committed          — o que a engine faz hoje (recurso + heap implicito por recurso)
//   2. placed/heap pronto — heap unico criado ANTES do cronometro; so o objeto de recurso
//                           entra na conta. E o TETO do que qualquer sub-alocador (D3D12MA
//                           inclusive) pode economizar, porque nenhum deles cria menos que
//                           zero heap no caminho quente.
//   3. placed/heap dentro — heap grande criado DENTRO do cronometro, para dar a ordem de
//                           grandeza do custo de UM heap grande contra N pequenos.
//
// O D3D12MA nao entra: ele fica ENTRE (1) e (2), e vendorar a lib para descobrir onde
// exatamente so faz sentido depois que (2) mostrar que ha o que ganhar.
//
// Standalone de proposito — nao linka a SmileEngine. Cria o proprio device.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

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

    // Fallback quando nao ha captura. APROXIMA a forma do conjunto do resize — e so isso.
    // A primeira versao deste benchmark dizia "~600 MB" e somava 787 MB (30% acima do real),
    // o que preservava o sinal mas invalidava coeficiente por MB. Por isso o main avisa em
    // alto e bom som quando esta neste caminho.
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
        double Ms    = 0.0;
        UINT64 Bytes = 0;
        bool   Ok    = false;
    };

    FRun RunCommitted(ID3D12Device* Dev, const std::vector<D3D12_RESOURCE_DESC>& Descs) {
        std::vector<ComPtr<ID3D12Resource>> Keep(Descs.size());
        D3D12_HEAP_PROPERTIES Hp{}; Hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        FRun R;
        const auto Start = Clock::now();
        for (size_t i = 0; i < Descs.size(); ++i) {
            if (FAILED(Dev->CreateCommittedResource(&Hp, D3D12_HEAP_FLAG_NONE, &Descs[i],
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&Keep[i]))))
                return R;
        }
        R.Ms = MsSince(Start);
        for (const auto& D : Descs)
            R.Bytes += Dev->GetResourceAllocationInfo(0, 1, &D).SizeInBytes;
        R.Ok = true;
        return R;
    }

    // HeapInside = o heap entra no cronometro. Fora dele, a medida isola o custo do OBJETO
    // de recurso, que e o piso que nenhum allocator remove.
    FRun RunPlaced(ID3D12Device* Dev, const std::vector<D3D12_RESOURCE_DESC>& Descs,
                   bool HeapInside) {
        // Layout primeiro: offset de placed tem que respeitar o Alignment que o device pede.
        std::vector<UINT64> Offsets(Descs.size());
        UINT64 Total = 0;
        for (size_t i = 0; i < Descs.size(); ++i) {
            const auto Info = Dev->GetResourceAllocationInfo(0, 1, &Descs[i]);
            Total        = (Total + Info.Alignment - 1) & ~(Info.Alignment - 1);
            Offsets[i]   = Total;
            Total       += Info.SizeInBytes;
        }

        D3D12_HEAP_DESC Hd{};
        Hd.SizeInBytes           = Total;
        Hd.Properties.Type       = D3D12_HEAP_TYPE_DEFAULT;
        Hd.Alignment             = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        // Tier 2: um heap serve buffer, textura comum e RT/DS. Em Tier 1 isto falha, e e por
        // isso que o main checa o tier antes de chamar aqui.
        Hd.Flags                 = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

        FRun R;
        R.Bytes = Total;

        ComPtr<ID3D12Heap> Heap;
        if (!HeapInside && !Check(Dev->CreateHeap(&Hd, IID_PPV_ARGS(&Heap)), "CreateHeap"))
            return R;

        std::vector<ComPtr<ID3D12Resource>> Keep(Descs.size());
        const auto Start = Clock::now();
        if (HeapInside && FAILED(Dev->CreateHeap(&Hd, IID_PPV_ARGS(&Heap)))) return R;
        for (size_t i = 0; i < Descs.size(); ++i) {
            if (FAILED(Dev->CreatePlacedResource(Heap.Get(), Offsets[i], &Descs[i],
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&Keep[i]))))
                return R;
        }
        R.Ms = MsSince(Start);
        R.Ok = true;
        return R;
    }

    double Median(std::vector<double> V) {
        if (V.empty()) return 0.0;
        std::sort(V.begin(), V.end());
        return V[V.size() / 2];
    }

    void Report(const char* Name, const std::vector<double>& Samples, size_t N, UINT64 Bytes,
                double BaselineMs) {
        if (Samples.empty()) { std::printf("  %-22s FALHOU\n", Name); return; }
        const double Med = Median(Samples);
        std::printf("  %-22s %7.2f ms   %6.3f ms/rec   %7.1f MB   [%.1f-%.1f]",
                    Name, Med, Med / static_cast<double>(N),
                    static_cast<double>(Bytes) / (1024.0 * 1024.0),
                    *std::min_element(Samples.begin(), Samples.end()),
                    *std::max_element(Samples.begin(), Samples.end()));
        if (BaselineMs > 0.0)
            std::printf("   %+6.1f%%", (Med / BaselineMs - 1.0) * 100.0);
        std::printf("\n");
    }

} // namespace

int main(int argc, char** argv) {
    // Uso:
    //   SmileAllocBench <captura.txt>     <- preferido; descritores reais da engine
    //   SmileAllocBench <largura> <altura> <- fallback aproximado
    const char* CapturePath = nullptr;
    UINT W = 1573, H = 804;
    if (argc == 2)      CapturePath = argv[1];
    else if (argc >= 3) { W = std::atoi(argv[1]); H = std::atoi(argv[2]); }

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

    // Uma passada descartada: a primeira alocacao de VRAM do processo paga inicializacao do
    // driver que nao se repete, e ela cairia inteira no primeiro caminho medido.
    (void)RunCommitted(Device.Get(), Descs);

    // Ordem ALTERNADA entre iteracoes. Com ordem fixa, o caminho medido primeiro paga o
    // estado de pagina deixado pelo anterior sempre do mesmo jeito, e a razao entre eles
    // herda esse vies — foi o que fez as faixas relatadas (-97/-98%) nao baterem com uma
    // execucao independente (-99,1%).
    std::vector<double> Committed, PlacedReady, PlacedFull;
    UINT64 Bytes = 0;
    for (int It = 0; It < kIterations; ++It) {
        auto DoCommitted = [&] { const FRun R = RunCommitted(Device.Get(), Descs);
                                 if (R.Ok) { Committed.push_back(R.Ms); Bytes = R.Bytes; } };
        auto DoReady     = [&] { const FRun R = RunPlaced(Device.Get(), Descs, false);
                                 if (R.Ok) PlacedReady.push_back(R.Ms); };
        auto DoFull      = [&] { const FRun R = RunPlaced(Device.Get(), Descs, true);
                                 if (R.Ok) PlacedFull.push_back(R.Ms); };
        if (It % 3 == 0)      { DoCommitted(); DoReady(); DoFull(); }
        else if (It % 3 == 1) { DoFull(); DoCommitted(); DoReady(); }
        else                  { DoReady(); DoFull(); DoCommitted(); }
    }

    std::printf("\n%-24s %10s %14s %11s %14s %8s\n",
                "", "mediana", "por recurso", "bytes", "[min-max]", "vs comm");
    const double Base = Median(Committed);
    Report("committed", Committed, Descs.size(), Bytes, 0.0);
    Report("placed (heap pronto)", PlacedReady, Descs.size(), Bytes, Base);
    Report("placed (heap dentro)", PlacedFull, Descs.size(), Bytes, Base);

    if (!Committed.empty() && !PlacedReady.empty()) {
        const double Ready = Median(PlacedReady);
        std::printf(
            "\nMedianas de %d iteracoes em ordem alternada.\n"
            "TETO de economia de um sub-alocador: %.3f ms/recurso (%.1f%% do custo atual).\n"
            "Piso restante (%.3f ms/recurso) e o objeto de recurso, que placed NAO remove.\n",
            kIterations, (Base - Ready) / static_cast<double>(Descs.size()),
            (Base - Ready) / Base * 100.0, Ready / static_cast<double>(Descs.size()));
    }
    return 0;
}
