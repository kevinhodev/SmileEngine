// Executa o resolvedor de identidade temporal DE VERDADE, em WARP.
//
// Substitui o gate de strings que cobria isto antes. O gate so pegava mudancas que apagavam texto:
// trocar "||" por "&&" na comparacao de identidade mantem as duas substrings, passava verde, e o
// resolvedor passava a aceitar o ocupante novo de um slot reciclado -- exatamente o que a
// generation existe para impedir. Medido antes de escrever este arquivo.
//
// WARP e o rasterizador de software da Microsoft: nao exige placa, entao isto roda no CI. O que ele
// NAO cobre e a integracao no editor -- histórico ativo, frames em voo, custo real.
//
// Os dados vem das funcoes de preenchimento de PRODUCAO (BuildIdentityTable / BuildSlotRemap), e o
// shader e compilado pelo MESMO DXC dos shaders do jogo. A cadeia exercitada e a real.

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Smile/Scene/CapturedIdentity.h"
#include "Smile/Scene/GpuSceneHandle.h"

// ComPtr existe nos dois namespaces; qualificar evita a ambiguidade.
namespace wrl = Microsoft::WRL;
using namespace Smile;

namespace {
    int Failures = 0;

    void Check(bool Condition, std::string_view Message) {
        if (!Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << Message << '\n';
        }
    }

    void Hr(HRESULT H, const char* Onde) {
        if (FAILED(H)) {
            char Buf[32];
            std::snprintf(Buf, sizeof(Buf), " falhou (0x%08X)", static_cast<unsigned>(H));
            throw std::runtime_error(std::string(Onde) + Buf);
        }
    }

    std::vector<u8> LerArquivo(const char* Caminho) {
        std::ifstream F(Caminho, std::ios::binary | std::ios::ate);
        if (!F) return {};
        const std::streamsize N = F.tellg();
        F.seekg(0);
        std::vector<u8> Dados(static_cast<size_t>(N));
        F.read(reinterpret_cast<char*>(Dados.data()), N);
        return Dados;
    }

    struct FCaso {
        const char* Nome;
        u32         PreviousId;
        u32         SlotCapacity;
        u32         CurrentCount;
        bool        EsperaResolver;
        u32         EsperaIndice;
    };

    // Executor minimo: sobe os quatro buffers de entrada, despacha a sonda e le o resultado.
    struct FSonda {
        wrl::ComPtr<ID3D12Device>              Device;
        wrl::ComPtr<ID3D12CommandQueue>        Queue;
        wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
        wrl::ComPtr<ID3D12GraphicsCommandList> List;
        wrl::ComPtr<ID3D12RootSignature>       Root;
        wrl::ComPtr<ID3D12PipelineState>       Pipeline;
        wrl::ComPtr<ID3D12DescriptorHeap>      Heap;
        wrl::ComPtr<ID3D12Fence>               Fence;
        u64                               FenceValue = 0;
        UINT                              Step       = 0;

        wrl::ComPtr<ID3D12Resource> Upload(const void* Dados, u64 Bytes) {
            D3D12_HEAP_PROPERTIES HP{};
            HP.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC RD{};
            RD.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            RD.Width            = Bytes;
            RD.Height           = 1;
            RD.DepthOrArraySize = 1;
            RD.MipLevels        = 1;
            RD.SampleDesc.Count = 1;
            RD.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            wrl::ComPtr<ID3D12Resource> R;
            Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&R)), "CreateCommittedResource");
            if (Dados) {
                void* P = nullptr;
                Hr(R->Map(0, nullptr, &P), "Map");
                std::memcpy(P, Dados, static_cast<size_t>(Bytes));
                R->Unmap(0, nullptr);
            }
            return R;
        }

        // UAV nao e permitido em heap UPLOAD -- a saida precisa ser DEFAULT, e a leitura
        // volta por um buffer READBACK.
        wrl::ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE Tipo, u64 Bytes, bool ComUAV,
                                           D3D12_RESOURCE_STATES Estado) {
            D3D12_HEAP_PROPERTIES HP{};
            HP.Type = Tipo;
            D3D12_RESOURCE_DESC RD{};
            RD.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            RD.Width            = Bytes;
            RD.Height           = 1;
            RD.DepthOrArraySize = 1;
            RD.MipLevels        = 1;
            RD.SampleDesc.Count = 1;
            RD.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (ComUAV) RD.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            wrl::ComPtr<ID3D12Resource> R;
            Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD, Estado,
                                               nullptr, IID_PPV_ARGS(&R)), "CreateBuffer");
            return R;
        }

        void SRV(ID3D12Resource* R, u32 Elementos, u32 Stride, UINT Slot) {
            D3D12_SHADER_RESOURCE_VIEW_DESC D{};
            D.Format                     = DXGI_FORMAT_UNKNOWN;
            D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            D.Buffer.NumElements         = Elementos;
            D.Buffer.StructureByteStride = Stride;
            auto H = Heap->GetCPUDescriptorHandleForHeapStart();
            H.ptr += static_cast<SIZE_T>(Slot) * Step;
            Device->CreateShaderResourceView(R, &D, H);
        }
    };
}

int main() try {
    // ---- cena: [A,B,C,D] vira [B,C,E], com E reciclando o slot de A -----------------------------
    FGpuSceneSlots Slots;
    const FGpuSceneHandle A = Slots.Allocate();
    const FGpuSceneHandle B = Slots.Allocate();
    const FGpuSceneHandle C = Slots.Allocate();
    const FGpuSceneHandle D = Slots.Allocate();

    constexpr u32 kCapacidade = 8;
    constexpr u32 kSlots      = 8;

    std::vector<FGpuIdentityEntry> Anterior(kCapacidade);
    const std::vector<FGpuSceneHandle> ListaAnterior{ A, B, C, D };
    BuildIdentityTable(Anterior.data(), kCapacidade, 4u, [&](u32 i) { return ListaAnterior[i]; });

    // So A e aposentado: assim o unico slot livre e o dele, e E o recicla de forma
    // DETERMINISTICA (aposentar D junto deixava a escolha do free list decidir o cenario).
    FGpuFenceSet Fence{ 1, 1 };
    Slots.Retire(A, Fence);
    Slots.Collect(FGpuFenceSet{ 100, 100 });
    const FGpuSceneHandle E = Slots.Allocate();
    // D continua vivo no pool, mas FORA da lista de agora -- que e o que "sumiu da cena"
    // significa para o remap.

    // Ordem escolhida para C cair no INDICE 2: o caso da contagem precisa de um candidato que
    // seja indice pequeno e plausivel, e nao do kInvalidSlot que os removidos devolvem.
    const std::vector<FGpuSceneHandle> ListaAtual{ B, E, C };
    std::vector<FGpuIdentityEntry> Atual(kCapacidade);
    BuildIdentityTable(Atual.data(), kCapacidade, 3u, [&](u32 i) { return ListaAtual[i]; });
    std::vector<u32> Remap(kSlots);
    BuildSlotRemap(Remap.data(), kSlots, 3u, [&](u32 i) { return ListaAtual[i]; });

    const bool Reciclou = (E.Slot == A.Slot) && (E.Generation != A.Generation);
    Check(Reciclou, "o cenario reciclou o slot de A para E, que e o caso central");

    const FCaso kCasos[] = {
        { "sobrevivente que mudou de posicao: B era 1, agora e 0", 1u, kSlots, 3u, true,  0u },
        { "objeto removido nao resolve",                           3u, kSlots, 3u, false, 0u },
        { "slot RECICLADO nao resolve: so a generation separa",    0u, kSlots, 3u, false, 0u },
        { "posicao vazia da tabela (generation 0) nao resolve",    5u, kSlots, 3u, false, 0u },
        // Mesmo id do primeiro caso, mas com o remap curto demais para o slot dele: tem de
        // reprovar em vez de ler fora.
        { "slot alem da capacidade do remap nao resolve",          1u, 1u,     3u, false, 0u },
        // O remap aponta para o indice 2 (E), mas a contagem daquele frame e 2: o candidato e um
        // indice PEQUENO e plausivel, e so a comparacao com a contagem o reprova. Sem esse caso,
        // trocar o limite por "!= invalido" passava despercebido.
        { "candidato valido porem acima da contagem nao resolve",  2u, kSlots, 2u, false, 0u },
        { "e com a contagem certa esse mesmo id resolve",          2u, kSlots, 3u, true,  2u },
    };
    constexpr u32 kNumCasos = static_cast<u32>(std::size(kCasos));

    std::vector<u32> Ids; // pares (id, slotCapacity)
    for (const FCaso& Caso : kCasos) {
        Ids.push_back(Caso.PreviousId);
        Ids.push_back(Caso.SlotCapacity);
        Ids.push_back(Caso.CurrentCount);
        Ids.push_back(0u);
    }

    // ---- WARP ------------------------------------------------------------------------------------
    FSonda S;
    wrl::ComPtr<IDXGIFactory4> Factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)))) {
        std::cerr << "DXGI indisponivel -- teste pulado\n";
        return 0;
    }
    wrl::ComPtr<IDXGIAdapter> Warp;
    if (FAILED(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp))) ||
        FAILED(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&S.Device)))) {
        std::cerr << "WARP indisponivel -- teste pulado\n";
        return 0;
    }

    const std::vector<u8> Bytecode = LerArquivo(SMILE_RESOLVE_PROBE_CSO);
    Check(!Bytecode.empty(), "a sonda compilada foi encontrada");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_RESOLVE_PROBE_CSO << ")\n";
        return 1;
    }

    D3D12_DESCRIPTOR_RANGE Ranges[2]{};
    Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors = 4;
    Ranges[1].RangeType                    = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors               = 1;
    Ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER Params[3]{};
    Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (int i = 0; i < 2; ++i) {
        Params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        Params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        Params[i + 1].DescriptorTable.pDescriptorRanges   = &Ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC SigDesc{};
    SigDesc.NumParameters = 3;
    SigDesc.pParameters   = Params;
    wrl::ComPtr<ID3DBlob> Serial, Erro;
    Hr(D3D12SerializeRootSignature(&SigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Serial, &Erro),
       "SerializeRootSignature");
    Hr(S.Device->CreateRootSignature(0, Serial->GetBufferPointer(), Serial->GetBufferSize(),
                                     IID_PPV_ARGS(&S.Root)), "CreateRootSignature");

    D3D12_COMPUTE_PIPELINE_STATE_DESC PSO{};
    PSO.pRootSignature = S.Root.Get();
    PSO.CS             = { Bytecode.data(), Bytecode.size() };
    Hr(S.Device->CreateComputePipelineState(&PSO, IID_PPV_ARGS(&S.Pipeline)),
       "CreateComputePipelineState");

    D3D12_DESCRIPTOR_HEAP_DESC HD{};
    HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HD.NumDescriptors = 5;
    HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Hr(S.Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&S.Heap)), "CreateDescriptorHeap");
    S.Step = S.Device->GetDescriptorHandleIncrementSize(HD.Type);

    auto BufAnterior = S.Upload(Anterior.data(), Anterior.size() * sizeof(FGpuIdentityEntry));
    auto BufRemap    = S.Upload(Remap.data(),    Remap.size()    * sizeof(u32));
    auto BufAtual    = S.Upload(Atual.data(),    Atual.size()    * sizeof(FGpuIdentityEntry));
    auto BufIds      = S.Upload(Ids.data(),      Ids.size()      * sizeof(u32));
    const u64 SaidaBytes = static_cast<u64>(kNumCasos) * sizeof(u32) * 2;
    auto BufSaida   = S.Buffer(D3D12_HEAP_TYPE_DEFAULT, SaidaBytes, true,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto BufLeitura = S.Buffer(D3D12_HEAP_TYPE_READBACK, SaidaBytes, false,
                               D3D12_RESOURCE_STATE_COPY_DEST);

    S.SRV(BufAnterior.Get(), kCapacidade, sizeof(FGpuIdentityEntry), 0);
    S.SRV(BufRemap.Get(),    kSlots,      sizeof(u32),               1);
    S.SRV(BufAtual.Get(),    kCapacidade, sizeof(FGpuIdentityEntry), 2);
    S.SRV(BufIds.Get(),      kNumCasos,   sizeof(u32) * 4,           3);
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC UD{};
        UD.Format                     = DXGI_FORMAT_UNKNOWN;
        UD.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        UD.Buffer.NumElements         = kNumCasos;
        UD.Buffer.StructureByteStride = sizeof(u32) * 2;
        auto H = S.Heap->GetCPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<SIZE_T>(4) * S.Step;
        S.Device->CreateUnorderedAccessView(BufSaida.Get(), nullptr, &UD, H);
    }

    // 256 bytes de verdade: copiar 256 de um struct de 16 lia fora da pilha.
    alignas(16) u8 CB[256]{};
    const u32 Constantes[4]{ kSlots, 3u, kNumCasos, 0u };
    std::memcpy(CB, Constantes, sizeof(Constantes));
    auto BufCB = S.Upload(CB, sizeof(CB));

    D3D12_COMMAND_QUEUE_DESC QD{};
    Hr(S.Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&S.Queue)), "CreateCommandQueue");
    Hr(S.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&S.Allocator)),
       "CreateCommandAllocator");
    Hr(S.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, S.Allocator.Get(),
                                   S.Pipeline.Get(), IID_PPV_ARGS(&S.List)), "CreateCommandList");

    ID3D12DescriptorHeap* Heaps[] = { S.Heap.Get() };
    S.List->SetDescriptorHeaps(1, Heaps);
    S.List->SetComputeRootSignature(S.Root.Get());
    S.List->SetComputeRootConstantBufferView(0, BufCB->GetGPUVirtualAddress());
    auto GpuAt = [&](UINT i) {
        auto H = S.Heap->GetGPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<UINT64>(i) * S.Step;
        return H;
    };
    S.List->SetComputeRootDescriptorTable(1, GpuAt(0));
    S.List->SetComputeRootDescriptorTable(2, GpuAt(4));
    S.List->Dispatch(1, 1, 1);
    {
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = BufSaida.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        S.List->ResourceBarrier(1, &B);
        S.List->CopyResource(BufLeitura.Get(), BufSaida.Get());
    }
    Hr(S.List->Close(), "Close");

    ID3D12CommandList* Listas[] = { S.List.Get() };
    S.Queue->ExecuteCommandLists(1, Listas);
    Hr(S.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&S.Fence)), "CreateFence");
    Hr(S.Queue->Signal(S.Fence.Get(), 1), "Signal");
    if (S.Fence->GetCompletedValue() < 1) {
        HANDLE Evento = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Hr(S.Fence->SetEventOnCompletion(1, Evento), "SetEventOnCompletion");
        WaitForSingleObject(Evento, 20000);
        CloseHandle(Evento);
    }

    const u32* Saida = nullptr;
    Hr(BufLeitura->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u32**>(&Saida))), "Map saida");
    for (u32 i = 0; i < kNumCasos; ++i) {
        const bool Resolveu = Saida[i * 2] != 0;
        const u32  Indice   = Saida[i * 2 + 1];
        Check(Resolveu == kCasos[i].EsperaResolver, kCasos[i].Nome);
        if (kCasos[i].EsperaResolver && Resolveu) {
            Check(Indice == kCasos[i].EsperaIndice, "e devolveu o indice atual correto");
        }
    }
    BufLeitura->Unmap(0, nullptr);

    if (Failures == 0) {
        std::cout << "TemporalResolveWarp: " << kNumCasos << " casos executados em WARP, todos OK\n";
        return 0;
    }
    std::cerr << "TemporalResolveWarp: " << Failures << " falha(s)\n";
    return 1;
} catch (const std::exception& E) {
    std::cerr << "TemporalResolveWarp: excecao -- " << E.what() << '\n';
    return 2;
}
