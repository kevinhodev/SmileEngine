// Expansao de ranges na GPU, executada em WARP (Fase 4 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir, em ordem de gravidade:
//
//  1. o dispatch produzir uma lista DIFERENTE da referencia CPU. Enquanto nao houver consumidor,
//     uma divergencia nao aparece em lugar nenhum -- e quando houver, ela aparece como geometria
//     faltando ou desenhada duas vezes, longe da causa;
//  2. range unico e ranges sinteticos divergirem. E o que autoriza world cells a trocarem o
//     produtor sem tocar no consumidor;
//  3. os contadores mentirem. Slot invalido, linha vazia, linha RaytracingOnly e overflow tem de
//     ser CONTADOS, nao descartados em silencio -- regra 6 do plano;
//  4. os indices dos contadores divergirem entre o C++ e o HLSL. Sao dois espelhos de macros e
//     nada os liga em tempo de compilacao; aqui os quatro sao lidos de volta.
//
// O shader e o de PRODUCAO, compilado pelo MESMO DXC dos shaders do jogo. WARP nao exige placa.

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Scene/GpuSceneCandidates.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"

namespace wrl = Microsoft::WRL;
using namespace Smile;

namespace {
    int Failures = 0;

    void Check(bool _Condition, std::string_view _Message) {
        if (!_Condition) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << "\n";
        }
    }

    void CheckEq(u32 _Got, u32 _Want, std::string_view _Message) {
        if (_Got != _Want) {
            ++Failures;
            std::cerr << "  FAIL: " << _Message << " (obtido " << _Got << ", esperado " << _Want
                      << ")\n";
        }
    }

    void Hr(HRESULT _H, const char* _Onde) {
        if (FAILED(_H)) {
            char Buf[32];
            std::snprintf(Buf, sizeof(Buf), " falhou (0x%08X)", static_cast<unsigned>(_H));
            throw std::runtime_error(std::string(_Onde) + Buf);
        }
    }

    std::vector<u8> LerArquivo(const char* _Caminho) {
        std::ifstream F(_Caminho, std::ios::binary | std::ios::ate);
        if (!F) return {};
        const std::streamsize N = F.tellg();
        F.seekg(0);
        std::vector<u8> Dados(static_cast<size_t>(N));
        F.read(reinterpret_cast<char*>(Dados.data()), N);
        return Dados;
    }

    // Aparelho: device WARP, root signature de quatro SRV + dois UAV, e o PSO do shader de
    // producao. Cada Executar() e um dispatch independente com buffers proprios.
    struct FSonda {
        wrl::ComPtr<ID3D12Device>              Device;
        wrl::ComPtr<ID3D12CommandQueue>        Queue;
        wrl::ComPtr<ID3D12RootSignature>       Root;
        wrl::ComPtr<ID3D12PipelineState>       Pipeline;
        wrl::ComPtr<ID3D12Fence>               Fence;
        u64                                    FenceValue = 0;
        UINT                                   Step = 0;

        wrl::ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE _Tipo, u64 _Bytes, bool _ComUAV,
                                           D3D12_RESOURCE_STATES _Estado) {
            D3D12_HEAP_PROPERTIES HP{};
            HP.Type = _Tipo;
            D3D12_RESOURCE_DESC RD{};
            RD.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            RD.Width            = _Bytes;
            RD.Height           = 1;
            RD.DepthOrArraySize = 1;
            RD.MipLevels        = 1;
            RD.SampleDesc.Count = 1;
            RD.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (_ComUAV) RD.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            wrl::ComPtr<ID3D12Resource> R;
            const HRESULT H = Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD,
                                                              _Estado, nullptr, IID_PPV_ARGS(&R));
            // Uma criacao que falha DEPOIS de o device morrer diz "DEVICE_REMOVED" e esconde a
            // causa, que aconteceu num dispatch anterior. O motivo da remocao junto poupa a
            // rodada de bisseccao que este teste ja custou uma vez.
            if (FAILED(H)) {
                char Buf[64];
                std::snprintf(Buf, sizeof(Buf), " (device removido: 0x%08X)",
                              static_cast<unsigned>(Device->GetDeviceRemovedReason()));
                Hr(H, (std::string("CreateBuffer") + Buf).c_str());
            }
            Hr(H, "CreateBuffer");
            return R;
        }

        wrl::ComPtr<ID3D12Resource> Upload(const void* _Dados, u64 _Bytes) {
            auto R = Buffer(D3D12_HEAP_TYPE_UPLOAD, _Bytes, false,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
            if (_Dados) {
                void* P = nullptr;
                Hr(R->Map(0, nullptr, &P), "Map");
                std::memcpy(P, _Dados, static_cast<size_t>(_Bytes));
                R->Unmap(0, nullptr);
            }
            return R;
        }

        void Construir(const std::vector<u8>& _Bytecode) {
            wrl::ComPtr<IDXGIFactory4> Factory;
            Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1");
            wrl::ComPtr<IDXGIAdapter> Warp;
            Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter");
            Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)),
               "D3D12CreateDevice");

            D3D12_DESCRIPTOR_RANGE Ranges[2]{};
            Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            Ranges[0].NumDescriptors = 4;
            Ranges[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            Ranges[1].NumDescriptors = 2;

            D3D12_ROOT_PARAMETER Params[3]{};
            Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            for (int I = 0; I < 2; ++I) {
                Params[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                Params[I + 1].DescriptorTable.NumDescriptorRanges = 1;
                Params[I + 1].DescriptorTable.pDescriptorRanges   = &Ranges[I];
            }
            D3D12_ROOT_SIGNATURE_DESC SigDesc{};
            SigDesc.NumParameters = 3;
            SigDesc.pParameters   = Params;
            wrl::ComPtr<ID3DBlob> Serial, Erro;
            Hr(D3D12SerializeRootSignature(&SigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Serial, &Erro),
               "SerializeRootSignature");
            Hr(Device->CreateRootSignature(0, Serial->GetBufferPointer(), Serial->GetBufferSize(),
                                           IID_PPV_ARGS(&Root)), "CreateRootSignature");

            D3D12_COMPUTE_PIPELINE_STATE_DESC PSO{};
            PSO.pRootSignature = Root.Get();
            PSO.CS             = { _Bytecode.data(), _Bytecode.size() };
            Hr(Device->CreateComputePipelineState(&PSO, IID_PPV_ARGS(&Pipeline)),
               "CreateComputePipelineState");

            D3D12_COMMAND_QUEUE_DESC QD{};
            Hr(Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&Queue)), "CreateCommandQueue");
            Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence");
            Step = Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        struct FResultado {
            std::vector<u32> Slots;
            u32 Counters[GpuExpandCounterCount]{};
        };

        FResultado Executar(std::span<const FInstanceRange> _Ranges,
                            std::span<const u32> _CandidateIds,
                            std::span<const FGpuSceneInstanceMeta> _Meta, u32 _OutCapacity) {
            std::vector<u32> Bases;
            const u32 Total = BuildRangeOutputBases(_Ranges, Bases);

            // Piso de UM ELEMENTO, e nao de um byte. Um recurso de zero bytes e ilegal, mas um
            // recurso de 1 byte com um SRV de um elemento de 8 B tambem e — e esse desfaz o device
            // no dispatch seguinte, com a falha aparecendo longe da causa. O piso e sempre o
            // stride da view.
            auto BufRanges = Upload(_Ranges.data(),
                                    std::max<u64>(sizeof(FInstanceRange),
                                                  _Ranges.size() * sizeof(FInstanceRange)));
            auto BufBases  = Upload(Bases.data(), std::max<u64>(4, Bases.size() * sizeof(u32)));
            auto BufIds    = Upload(_CandidateIds.data(),
                                    std::max<u64>(4, _CandidateIds.size() * sizeof(u32)));
            auto BufMeta   = Upload(_Meta.data(),
                                    std::max<u64>(sizeof(FGpuSceneInstanceMeta),
                                                  _Meta.size() * sizeof(FGpuSceneInstanceMeta)));

            const u64 SaidaBytes = std::max<u64>(4, static_cast<u64>(_OutCapacity) * sizeof(u32));
            auto BufSaida = Buffer(D3D12_HEAP_TYPE_DEFAULT, SaidaBytes, true,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const u64 ContBytes = GpuExpandCounterCount * sizeof(u32);
            auto BufCont = Buffer(D3D12_HEAP_TYPE_DEFAULT, ContBytes, true,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            // Zeros vem de um upload copiado: um DEFAULT recem-criado nao promete conteudo, e
            // contadores lidos a partir de lixo dariam um resultado plausivel e falso.
            const u32 Zeros[GpuExpandCounterCount]{};
            auto BufZeros = Upload(Zeros, ContBytes);

            auto BufLeituraSaida = Buffer(D3D12_HEAP_TYPE_READBACK, SaidaBytes, false,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
            auto BufLeituraCont  = Buffer(D3D12_HEAP_TYPE_READBACK, ContBytes, false,
                                          D3D12_RESOURCE_STATE_COPY_DEST);

            wrl::ComPtr<ID3D12DescriptorHeap> Heap;
            D3D12_DESCRIPTOR_HEAP_DESC HD{};
            HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            HD.NumDescriptors = 6;
            HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Hr(Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");

            auto CpuAt = [&](UINT I) {
                auto H = Heap->GetCPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<SIZE_T>(I) * Step;
                return H;
            };
            auto SRV = [&](ID3D12Resource* R, u32 Elementos, u32 Stride, UINT Slot) {
                D3D12_SHADER_RESOURCE_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max(1u, Elementos);
                D.Buffer.StructureByteStride = Stride;
                Device->CreateShaderResourceView(R, &D, CpuAt(Slot));
            };
            auto UAV = [&](ID3D12Resource* R, u32 Elementos, UINT Slot) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max(1u, Elementos);
                D.Buffer.StructureByteStride = sizeof(u32);
                Device->CreateUnorderedAccessView(R, nullptr, &D, CpuAt(Slot));
            };

            SRV(BufRanges.Get(), static_cast<u32>(_Ranges.size()), sizeof(FInstanceRange), 0);
            SRV(BufBases.Get(), static_cast<u32>(Bases.size()), sizeof(u32), 1);
            SRV(BufIds.Get(), static_cast<u32>(_CandidateIds.size()), sizeof(u32), 2);
            SRV(BufMeta.Get(), static_cast<u32>(_Meta.size()), sizeof(FGpuSceneInstanceMeta), 3);
            UAV(BufSaida.Get(), _OutCapacity, 4);
            UAV(BufCont.Get(), GpuExpandCounterCount, 5);

            alignas(16) u8 CB[256]{};
            const u32 Params4[4]{ Total, static_cast<u32>(_Ranges.size()), _OutCapacity,
                                  static_cast<u32>(_Meta.size()) };
            std::memcpy(CB, Params4, sizeof(Params4));
            auto BufCB = Upload(CB, sizeof(CB));

            wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
            wrl::ComPtr<ID3D12GraphicsCommandList> List;
            Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&Allocator)), "CreateCommandAllocator");
            Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                         Pipeline.Get(), IID_PPV_ARGS(&List)), "CreateCommandList");

            List->CopyBufferRegion(BufCont.Get(), 0, BufZeros.Get(), 0, ContBytes);
            auto Transicao = [&](ID3D12Resource* R, D3D12_RESOURCE_STATES A,
                                 D3D12_RESOURCE_STATES B) {
                D3D12_RESOURCE_BARRIER Bar{};
                Bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                Bar.Transition.pResource   = R;
                Bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                Bar.Transition.StateBefore = A;
                Bar.Transition.StateAfter  = B;
                List->ResourceBarrier(1, &Bar);
            };
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ID3D12DescriptorHeap* Heaps[] = { Heap.Get() };
            List->SetDescriptorHeaps(1, Heaps);
            List->SetComputeRootSignature(Root.Get());
            List->SetComputeRootConstantBufferView(0, BufCB->GetGPUVirtualAddress());
            auto GpuAt = [&](UINT I) {
                auto H = Heap->GetGPUDescriptorHandleForHeapStart();
                H.ptr += static_cast<UINT64>(I) * Step;
                return H;
            };
            List->SetComputeRootDescriptorTable(1, GpuAt(0));
            List->SetComputeRootDescriptorTable(2, GpuAt(4));
            if (Total > 0) List->Dispatch((Total + 63u) / 64u, 1, 1);

            Transicao(BufSaida.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(BufLeituraSaida.Get(), BufSaida.Get());
            List->CopyResource(BufLeituraCont.Get(), BufCont.Get());
            Hr(List->Close(), "Close");

            ID3D12CommandList* Listas[] = { List.Get() };
            Queue->ExecuteCommandLists(1, Listas);
            ++FenceValue;
            Hr(Queue->Signal(Fence.Get(), FenceValue), "Signal");
            if (Fence->GetCompletedValue() < FenceValue) {
                struct FEvent {
                    HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    ~FEvent() { if (Value) CloseHandle(Value); }
                } Evento;
                if (!Evento.Value) Hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
                Hr(Fence->SetEventOnCompletion(FenceValue, Evento.Value), "SetEventOnCompletion");
                if (WaitForSingleObject(Evento.Value, 30000) != WAIT_OBJECT_0)
                    throw std::runtime_error("dispatch de expansao nao terminou");
            }

            {
                wrl::ComPtr<ID3D12InfoQueue> IQ;
                if (SUCCEEDED(Device.As(&IQ))) {
                    const UINT64 N = IQ->GetNumStoredMessages();
                    for (UINT64 I = 0; I < N; ++I) {
                        SIZE_T Bytes = 0;
                        IQ->GetMessage(I, nullptr, &Bytes);
                        std::vector<u8> Buf(Bytes);
                        auto* M = reinterpret_cast<D3D12_MESSAGE*>(Buf.data());
                        IQ->GetMessage(I, M, &Bytes);
                        std::cerr << "  D3D12: " << M->pDescription << '\n';
                    }
                    IQ->ClearStoredMessages();
                }
            }

            FResultado Saida;
            Saida.Slots.resize(_OutCapacity);
            if (_OutCapacity > 0) {
                void* P = nullptr;
                Hr(BufLeituraSaida->Map(0, nullptr, &P), "Map(saida)");
                std::memcpy(Saida.Slots.data(), P, _OutCapacity * sizeof(u32));
                BufLeituraSaida->Unmap(0, nullptr);
            }
            {
                void* P = nullptr;
                Hr(BufLeituraCont->Map(0, nullptr, &P), "Map(contadores)");
                std::memcpy(Saida.Counters, P, ContBytes);
                BufLeituraCont->Unmap(0, nullptr);
            }
            return Saida;
        }
    };

    // Tabela de meta com N linhas VIVAS. A linha 0 fica vazia de proposito (generation 0), que e a
    // convencao da tabela real: e a unica maneira de o teste ter um slot invalido de verdade.
    std::vector<FGpuSceneInstanceMeta> MetaViva(u32 _Linhas) {
        std::vector<FGpuSceneInstanceMeta> Meta(_Linhas);
        for (u32 I = 1; I < _Linhas; ++I) {
            Meta[I].MeshId     = I;
            Meta[I].MaterialId = I;
            Meta[I].Generation = I + 1u;
            Meta[I].Flags      = GpuSceneInstanceVisible;
        }
        return Meta;
    }
}

int main() try {
    std::cout << "Smile.GpuSceneExpandWarp\n";

    const std::vector<u8> Bytecode = LerArquivo(SMILE_EXPAND_RANGES_CSO);
    Check(!Bytecode.empty(), "o shader de expansao compilado foi encontrado");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_EXPAND_RANGES_CSO << ")\n";
        return 1;
    }

    FSonda Sonda;
    Sonda.Construir(Bytecode);

    // Slots NAO contiguos e fora de ordem: se o consumidor voltar a interpretar o range como
    // intervalo de slots, a saida deixa de bater. Com uma lista identidade os dois passariam.
    const std::vector<u32> Ids{ 7u, 2u, 11u, 3u, 9u, 5u };
    const std::vector<FGpuSceneInstanceMeta> Meta = MetaViva(16u);
    const u32 Capacidade = static_cast<u32>(Ids.size());

    // --- range unico da cena plana --------------------------------------------------------------
    const FInstanceRange Unico[] = { { 0u, static_cast<u32>(Ids.size()) } };
    const FSonda::FResultado Gpu = Sonda.Executar(Unico, Ids, Meta, Capacidade);

    std::vector<u32> Referencia;
    CheckEq(ExpandCandidateRanges(Unico, Ids, Referencia), 0u, "o range unico nao e ignorado");
    Check(Gpu.Slots == Referencia, "a expansao da GPU casa com a referencia CPU");
    CheckEq(Gpu.Counters[GpuExpandCounterExpanded], Capacidade, "todos os candidatos passam");
    CheckEq(Gpu.Counters[GpuExpandCounterInvalidSlot], 0u, "nenhum slot invalido");
    CheckEq(Gpu.Counters[GpuExpandCounterRtOnly], 0u, "nenhuma linha RaytracingOnly alcancada");
    CheckEq(Gpu.Counters[GpuExpandCounterOverflow], 0u, "nada estoura a capacidade");

    // --- N ranges sinteticos cobrindo a mesma lista ---------------------------------------------
    //
    // O GATE. Tamanhos diferentes e um range VAZIO no meio, para o prefixo ter de lidar com dois
    // ranges que comecam no mesmo offset de saida.
    {
        const FInstanceRange Sinteticos[] = { { 0u, 2u }, { 2u, 0u }, { 2u, 3u }, { 5u, 1u } };
        const FSonda::FResultado Muitos = Sonda.Executar(Sinteticos, Ids, Meta, Capacidade);
        Check(Muitos.Slots == Gpu.Slots,
              "range unico e ranges sinteticos produzem a mesma lista expandida na GPU");
        CheckEq(Muitos.Counters[GpuExpandCounterExpanded], Capacidade,
                "e a mesma contagem de expandidos");
    }

    // --- slot invalido e linha vazia -------------------------------------------------------------
    //
    // Dois candidatos ruins: um alem da tabela e um apontando para a linha 0, que e vazia por
    // convencao. Os dois tem de virar sentinela e contador, e nunca um slot plausivel.
    {
        const std::vector<u32> Ruins{ 4u, 999u, 0u, 6u };
        const FInstanceRange R[] = { { 0u, 4u } };
        const FSonda::FResultado R4 = Sonda.Executar(R, Ruins, Meta, 4u);
        CheckEq(R4.Counters[GpuExpandCounterExpanded], 2u, "so os dois validos passam");
        CheckEq(R4.Counters[GpuExpandCounterInvalidSlot], 2u,
                "slot fora da tabela e linha vazia sao contados");
        CheckEq(R4.Slots[0], 4u, "o primeiro valido e escrito na propria posicao");
        CheckEq(R4.Slots[1], kGpuExpandNoSlot, "o slot fora da tabela vira sentinela");
        CheckEq(R4.Slots[2], kGpuExpandNoSlot, "a linha vazia vira sentinela");
        CheckEq(R4.Slots[3], 6u, "o segundo valido tambem e posicional");
    }

    // --- verificacao cruzada da exclusao RaytracingOnly -------------------------------------------
    //
    // O produtor CPU ja filtra; este contador existe para o dia em que ele parar de filtrar. Se
    // ficar mudo, o terreno passa a ser desenhado duas vezes sem nada denunciar.
    {
        std::vector<FGpuSceneInstanceMeta> ComProxy = Meta;
        ComProxy[5].Flags |= GpuSceneInstanceRaytracingOnly;
        const std::vector<u32> ComProxyIds{ 4u, 5u, 6u };
        const FInstanceRange R[] = { { 0u, 3u } };
        const FSonda::FResultado Res = Sonda.Executar(R, ComProxyIds, ComProxy, 3u);
        CheckEq(Res.Counters[GpuExpandCounterRtOnly], 1u,
                "a linha RaytracingOnly e contada, nao expandida");
        CheckEq(Res.Counters[GpuExpandCounterExpanded], 2u, "e nao entra na saida");
        CheckEq(Res.Slots[1], kGpuExpandNoSlot, "a posicao dela fica marcada");
    }

    // --- overflow ---------------------------------------------------------------------------------
    //
    // Capacidade menor que a lista. O que nao coube tem de ser CONTADO: um teto que descartasse em
    // silencio faz geometria sumir, que e o que a regra 6 do plano proibe.
    {
        const FSonda::FResultado Apertado = Sonda.Executar(Unico, Ids, Meta, 4u);
        CheckEq(Apertado.Counters[GpuExpandCounterExpanded], 4u, "so o que cabe e expandido");
        CheckEq(Apertado.Counters[GpuExpandCounterOverflow], 2u, "o que nao coube e contado");
    }

    // --- lista vazia -------------------------------------------------------------------------------
    //
    // Sem range nenhum o dispatch nao roda, e nenhum contador pode subir. Um shader que tratasse
    // RangeCount == 0 pela busca binaria leria RangeOutputBase[-1].
    {
        const FSonda::FResultado Vazio = Sonda.Executar({}, {}, Meta, 4u);
        CheckEq(Vazio.Counters[GpuExpandCounterExpanded], 0u, "lista vazia nao expande nada");
        CheckEq(Vazio.Counters[GpuExpandCounterInvalidSlot], 0u, "nem conta invalido");
    }

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
} catch (const std::exception& E) {
    std::cerr << "  FAIL: " << E.what() << "\n";
    return 1;
}
