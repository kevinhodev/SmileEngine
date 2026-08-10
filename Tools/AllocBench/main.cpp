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

    // Espelha a forma do conjunto que o RecreateInternalTargets da engine recria: 79 alvos
    // dependentes de resolucao, somando ~600 MB em 1573x804. Nao sao os descritores
    // capturados da engine — sao representativos. O que importa para a pergunta e o formato
    // e o tamanho, nao qual passe pediu.
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

    void Report(const char* Name, const FRun& R, size_t N, double BaselineMs) {
        if (!R.Ok) { std::printf("  %-22s FALHOU\n", Name); return; }
        std::printf("  %-22s %7.2f ms   %6.3f ms/recurso   %7.1f MB",
                    Name, R.Ms, R.Ms / static_cast<double>(N),
                    static_cast<double>(R.Bytes) / (1024.0 * 1024.0));
        if (BaselineMs > 0.0)
            std::printf("   %+6.1f%%", (R.Ms / BaselineMs - 1.0) * 100.0);
        std::printf("\n");
    }

} // namespace

int main(int argc, char** argv) {
    UINT W = 1573, H = 804;               // mesma resolucao do resize medido na engine
    if (argc >= 3) { W = std::atoi(argv[1]); H = std::atoi(argv[2]); }

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

    const auto Descs = BuildResizeSet(W, H);
    std::printf("\nConjunto: %zu recursos em %ux%u\n", Descs.size(), W, H);

    // Uma passada descartada antes de medir: a primeira alocacao de VRAM do processo paga
    // inicializacao do driver que nao se repete, e ela cairia toda no primeiro caminho.
    (void)RunCommitted(Device.Get(), Descs);

    std::printf("\n%-24s %10s %20s %12s %10s\n", "", "total", "por recurso", "bytes", "vs committed");
    const FRun Committed = RunCommitted(Device.Get(), Descs);
    Report("committed", Committed, Descs.size(), 0.0);

    const FRun PlacedReady = RunPlaced(Device.Get(), Descs, /*HeapInside*/ false);
    Report("placed (heap pronto)", PlacedReady, Descs.size(), Committed.Ms);

    const FRun PlacedFull = RunPlaced(Device.Get(), Descs, /*HeapInside*/ true);
    Report("placed (heap dentro)", PlacedFull, Descs.size(), Committed.Ms);

    if (Committed.Ok && PlacedReady.Ok) {
        const double SavedPerResource =
            (Committed.Ms - PlacedReady.Ms) / static_cast<double>(Descs.size());
        std::printf(
            "\nTETO de economia de um sub-alocador: %.3f ms/recurso (%.1f%% do custo atual).\n"
            "O piso restante (%.3f ms/recurso) e o objeto de recurso, que placed NAO remove.\n",
            SavedPerResource, (Committed.Ms - PlacedReady.Ms) / Committed.Ms * 100.0,
            PlacedReady.Ms / static_cast<double>(Descs.size()));
    }
    return 0;
}
