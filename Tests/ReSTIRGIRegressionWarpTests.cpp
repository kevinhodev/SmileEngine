#include <limits>
// Executes production normal packing, reconstruction and mesh coverage helpers in WARP.
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

#include <cmath>
#include "Smile/Graphics/Lighting/MeshLights.h"

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

    struct F4 { float x, y, z, w; };
    struct FCaso { F4 Input; F4 Expected; float Tolerance; };


    // Upload inputs, dispatch production helpers, read back results.
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
    std::vector<FCaso> Cases;
    const F4 Normals[] = {
        {1,0,0,0}, {-1,0,0,0}, {0,1,0,0}, {0,-1,0,0}, {0,0,1,0}, {0,0,-1,0},
        {1,0,-1,0}, {-1,0,-1,0}, {0,1,-1,0}, {0,-1,-1,0}, {0,0,0,0},
        {0.000001f,-1,-1,0}, {-0.000001f,1,-1,0}, {1,1,-0.000001f,0}
    };
    for (u32 Mode = 0; Mode < 3; ++Mode) for (auto N : Normals) {
        const float L = std::sqrt(N.x*N.x + N.y*N.y + N.z*N.z);
        F4 Expected = L > 0 ? F4{N.x/L,N.y/L,N.z/L,0} : F4{0,0,1,0};
        N.w = static_cast<float>(Mode);
        Cases.push_back({N, Expected, Mode == 1 ? 0.005f : 0.0001f});
    }
    for (u32 Frames : {1u, 4u, 16u, 64u, 256u}) {
        const float Remaining = std::pow(0.75f, static_cast<float>(Frames));
        Cases.push_back({{static_cast<float>(Frames),1,0,3},
                         {Remaining,Remaining,Remaining,Remaining},0.00001f});
        const float Response = 1.0f - Remaining;
        Cases.push_back({{static_cast<float>(Frames),0,1,3},
                         {Response,Response,Response,Response},0.00001f});
    }
    std::vector<FMeshLightTaskGPU> Tasks(3);
    Tasks[0].InstanceIndex=2; Tasks[0].TriangleCount=5; Tasks[0].LightOffset=0;
    Tasks[1].InstanceIndex=7; Tasks[1].TriangleCount=1; Tasks[1].LightOffset=5;
    Tasks[2].InstanceIndex=10; Tasks[2].TriangleCount=1; Tasks[2].LightOffset=6;
    std::vector<FTriangleLightGPU> Lights(7);
    for (auto& L : Lights) { L.Edges0=0x00003c00; L.Edges1=0x3c000000;
                            L.Radiance=0x80000100; L.Flux=1; }
    Lights[1].Flux=0; // not sampled, even without compaction
    Lights[2].Edges0=0; // degenerates after fp16 packing
    Lights[3].Radiance=0; // flux is not enough if packed emission is zero
    Lights[4].Flux=std::numeric_limits<float>::infinity();
    auto Coverage = [&](float Instance, float Tri, float Count, bool Expected) {
        Cases.push_back({{Instance,Tri,Count,4},{Expected?1.0f:0.0f,0,0,0},0});
    };
    Coverage(2,0,3,true); Coverage(7,0,3,true); Coverage(10,0,3,true);
    Coverage(1,0,3,false); Coverage(6,0,3,false); Coverage(11,0,3,false);
    for (int Tri=1; Tri<=5; ++Tri) Coverage(2,static_cast<float>(Tri),3,false);
    Coverage(2,0,0,false); Coverage(7,1,3,false);
    const u32 kNumCasos = static_cast<u32>(Cases.size());
    std::vector<F4> Inputs;
    for (const auto& C : Cases) Inputs.push_back(C.Input);

    // ---- WARP ------------------------------------------------------------------------------------
    FSonda S;
    wrl::ComPtr<IDXGIFactory4> Factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1 (WARP obrigatorio)");
    wrl::ComPtr<IDXGIAdapter> Warp;
    Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter (WARP obrigatorio)");
    Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&S.Device)),
       "D3D12CreateDevice (WARP obrigatorio)");

    const std::vector<u8> Bytecode = LerArquivo(SMILE_RESTIR_GI_PROBE_CSO);
    Check(!Bytecode.empty(), "a sonda compilada foi encontrada");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_RESTIR_GI_PROBE_CSO << ")\n";
        return 1;
    }

    D3D12_DESCRIPTOR_RANGE Ranges[2]{};
    Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors = 3;
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
    HD.NumDescriptors = 4;
    HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Hr(S.Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&S.Heap)), "CreateDescriptorHeap");
    S.Step = S.Device->GetDescriptorHandleIncrementSize(HD.Type);

    auto BufInputs = S.Upload(Inputs.data(), Inputs.size()*sizeof(F4));
    auto BufTasks = S.Upload(Tasks.data(), Tasks.size()*sizeof(FMeshLightTaskGPU));
    auto BufLights = S.Upload(Lights.data(), Lights.size()*sizeof(FTriangleLightGPU));
    const u64 SaidaBytes = static_cast<u64>(kNumCasos) * sizeof(F4);
    auto BufSaida   = S.Buffer(D3D12_HEAP_TYPE_DEFAULT, SaidaBytes, true,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto BufLeitura = S.Buffer(D3D12_HEAP_TYPE_READBACK, SaidaBytes, false,
                               D3D12_RESOURCE_STATE_COPY_DEST);

    S.SRV(BufInputs.Get(), kNumCasos, sizeof(F4), 0);
    S.SRV(BufTasks.Get(), static_cast<u32>(Tasks.size()), sizeof(FMeshLightTaskGPU), 1);
    S.SRV(BufLights.Get(), static_cast<u32>(Lights.size()), sizeof(FTriangleLightGPU), 2);
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC UD{};
        UD.Format                     = DXGI_FORMAT_UNKNOWN;
        UD.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        UD.Buffer.NumElements         = kNumCasos;
        UD.Buffer.StructureByteStride = sizeof(F4);
        auto H = S.Heap->GetCPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<SIZE_T>(3) * S.Step;
        S.Device->CreateUnorderedAccessView(BufSaida.Get(), nullptr, &UD, H);
    }

    // 256 bytes de verdade: copiar 256 de um struct de 16 lia fora da pilha.
    alignas(16) u8 CB[256]{};
    const u32 Constantes[4]{ kNumCasos, 0u, 0u, 0u };
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
    S.List->SetComputeRootDescriptorTable(2, GpuAt(3));
    S.List->Dispatch((kNumCasos + 63u)/64u, 1, 1);
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
        struct FEvent {
            HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ~FEvent() { if (Value) CloseHandle(Value); }
        } Evento;
        if (!Evento.Value) Hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
        Hr(S.Fence->SetEventOnCompletion(1, Evento.Value), "SetEventOnCompletion");
        const DWORD Wait = WaitForSingleObject(Evento.Value, 20000);
        if (Wait == WAIT_FAILED) Hr(HRESULT_FROM_WIN32(GetLastError()), "WaitForSingleObject");
        if (Wait != WAIT_OBJECT_0) throw std::runtime_error("timeout aguardando o readback WARP");
    }
    const UINT64 Completed = S.Fence->GetCompletedValue();
    if (Completed == UINT64_MAX) Hr(S.Device->GetDeviceRemovedReason(), "WARP device removed");
    if (Completed == UINT64_MAX || Completed < 1)
        throw std::runtime_error("readback WARP sem conclusao confirmada da fence");

    const F4* Saida = nullptr;
    Hr(BufLeitura->Map(0, nullptr, reinterpret_cast<void**>(const_cast<F4**>(&Saida))), "Map output");
    for (u32 i=0; i<kNumCasos; ++i) {
        const float* Actual = &Saida[i].x;
        const float* Expected = &Cases[i].Expected.x;
        for (u32 c=0; c<4; ++c) {
            if (!std::isfinite(Actual[c]) || std::abs(Actual[c]-Expected[c]) > Cases[i].Tolerance) {
                ++Failures;
                std::cerr << "FAIL case " << i << " channel " << c << ": " << Actual[c]
                          << " expected " << Expected[c] << "\n";
            }
        }
    }
    BufLeitura->Unmap(0, nullptr);

    if (Failures == 0) {
        std::cout << "ReSTIRGIRegressionWarp: " << kNumCasos << " casos executados em WARP, todos OK\n";
        return 0;
    }
    std::cerr << "ReSTIRGIRegressionWarp: " << Failures << " falha(s)\n";
    return 1;
} catch (const std::exception& E) {
    std::cerr << "ReSTIRGIRegressionWarp: excecao -- " << E.what() << '\n';
    return 2;
}
