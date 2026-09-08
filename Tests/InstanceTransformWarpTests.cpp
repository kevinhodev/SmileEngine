// Paridade da composicao de transform, executada em WARP (Fase 4 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir: a Fase 4 tira do CBV por draw as quatro matrizes que o
// vertex shader consome e passa a compo-las DENTRO do shader, a partir das duas colunas canonicas
// mais as tres de view. O risco nao e de layout — e de ORDEM DO PRODUTO.
//
// `mul(Model, ViewProj)` e `mul(ViewProj, Model)` compilam os dois, desenham os dois, e o segundo
// poe a cena inteira num lugar errado que uma comparacao estatica de imagem pode ate nao separar de
// "a camera esta noutro lugar". Com `PrevMVP` e pior: a troca so aparece EM MOVIMENTO, como
// smear/ghosting, e o suspeito natural seria o TAA.
//
// Os dois lados sao os de PRODUCAO: `Smile::ComposeObjectTransforms`, que o BuildDrawLists chama, e
// `Shaders/Scene/InstanceTransform.hlsli`, que o vertex shader inclui. Nenhum dos dois e reescrito
// aqui.
//
// WARP nao exige placa. O que ele NAO cobre e a leitura pelo heap diretamente indexado, que e do
// vertex shader de producao e exige SM 6.6 mais root signature propria — isso fica para o A/B em
// GPU, e esta anotado no progresso da fase.

#include <cmath>
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

#include "Smile/Graphics/Scene/GpuInstanceTransform.h"
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

    // Tolerancia RELATIVA, e nao igualdade de bits. A CPU e a WARP nao precisam contrair os mesmos
    // produtos em FMA, e um epsilon absoluto reprovaria coordenadas de mundo grandes enquanto
    // aceitaria erro grosseiro perto da origem. 1e-5 relativo separa arredondamento de troca de
    // ordem por varias ordens de grandeza: uma matriz transposta erra o valor INTEIRO.
    bool Perto(f32 _A, f32 _B) {
        const f32 Escala = std::max({ 1.0f, std::fabs(_A), std::fabs(_B) });
        return std::fabs(_A - _B) <= 1e-5f * Escala;
    }

    bool MatrizIgual(const Mat44& _A, const f32* _B) {
        for (u32 R = 0; R < 4; ++R)
            for (u32 C = 0; C < 4; ++C)
                if (!Perto(_A.M[R][C], _B[R * 4u + C])) return false;
        return true;
    }

    // Matriz de teste com valores TODOS distintos e nenhum zero: uma transposicao ou uma troca de
    // ordem tem de mudar todo elemento. Com uma matriz cheia de 0/1 as duas ordens coincidiriam em
    // metade dos elementos e o teste passaria pela metade errada.
    Mat44 Densa(f32 _Semente) {
        Mat44 M{};
        for (u32 R = 0; R < 4; ++R)
            for (u32 C = 0; C < 4; ++C)
                M.M[R][C] = _Semente + static_cast<f32>(R) * 0.37f + static_cast<f32>(C) * 1.13f;
        return M;
    }

    constexpr u32 kInstancias = 4;
}

int main() try {
    std::cout << "Smile.InstanceTransformWarp\n";

    const std::vector<u8> Bytecode = LerArquivo(SMILE_INSTANCE_TRANSFORM_PROBE_CSO);
    Check(!Bytecode.empty(), "a sonda compilada foi encontrada");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_INSTANCE_TRANSFORM_PROBE_CSO << ")\n";
        return 1;
    }

    // --- dados -----------------------------------------------------------------------------------
    FGpuViewMatrices View;
    View.ViewProj               = Densa(1.0f);
    View.ViewProjUnjittered     = Densa(5.0f);
    View.PrevViewProjUnjittered = Densa(9.0f);

    std::vector<FGpuSceneInstanceTransform> Current(kInstancias), Previous(kInstancias);
    for (u32 I = 0; I < kInstancias; ++I) {
        Current[I].Value  = Densa(20.0f + static_cast<f32>(I));
        // A ultima instancia tem previous IGUAL ao current: e o caso "sem historico", em que o
        // motion vector do objeto tem de zerar. Ele so vale alguma coisa porque as outras tres
        // tem previous diferente — se todas fossem iguais, trocar PrevMVP por CurMVPNoJitter
        // passaria.
        Previous[I].Value = (I + 1u == kInstancias) ? Current[I].Value
                                                    : Densa(40.0f + static_cast<f32>(I));
    }

    // --- WARP --------------------------------------------------------------------------------
    wrl::ComPtr<ID3D12Device> Device;
    {
        wrl::ComPtr<IDXGIFactory4> Factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1");
        wrl::ComPtr<IDXGIAdapter> Warp;
        Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter");
        Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)),
           "D3D12CreateDevice");
    }

    auto Buffer = [&](D3D12_HEAP_TYPE _Tipo, u64 _Bytes, bool _ComUAV,
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
        Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD, _Estado, nullptr,
                                           IID_PPV_ARGS(&R)), "CreateBuffer");
        return R;
    };
    auto Upload = [&](const void* _Dados, u64 _Bytes) {
        auto R = Buffer(D3D12_HEAP_TYPE_UPLOAD, _Bytes, false, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* P = nullptr;
        Hr(R->Map(0, nullptr, &P), "Map");
        std::memcpy(P, _Dados, static_cast<size_t>(_Bytes));
        R->Unmap(0, nullptr);
        return R;
    };

    D3D12_DESCRIPTOR_RANGE Ranges[2]{};
    Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors = 2;
    Ranges[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    Ranges[1].NumDescriptors = 1;

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

    wrl::ComPtr<ID3D12RootSignature> Root;
    {
        wrl::ComPtr<ID3DBlob> Serial, Erro;
        Hr(D3D12SerializeRootSignature(&SigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Serial, &Erro),
           "SerializeRootSignature");
        Hr(Device->CreateRootSignature(0, Serial->GetBufferPointer(), Serial->GetBufferSize(),
                                       IID_PPV_ARGS(&Root)), "CreateRootSignature");
    }

    wrl::ComPtr<ID3D12PipelineState> Pipeline;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC PSO{};
        PSO.pRootSignature = Root.Get();
        PSO.CS             = { Bytecode.data(), Bytecode.size() };
        Hr(Device->CreateComputePipelineState(&PSO, IID_PPV_ARGS(&Pipeline)),
           "CreateComputePipelineState");
    }

    wrl::ComPtr<ID3D12DescriptorHeap> Heap;
    D3D12_DESCRIPTOR_HEAP_DESC HD{};
    HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HD.NumDescriptors = 3;
    HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Hr(Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");
    const UINT Step = Device->GetDescriptorHandleIncrementSize(HD.Type);

    const u64 TransformBytes =
        static_cast<u64>(kInstancias) * sizeof(FGpuSceneInstanceTransform);
    auto BufCurrent  = Upload(Current.data(), TransformBytes);
    auto BufPrevious = Upload(Previous.data(), TransformBytes);

    constexpr u32 kFloat4sPorInstancia = 16u;
    const u64 SaidaBytes = static_cast<u64>(kInstancias) * kFloat4sPorInstancia * 4u * sizeof(f32);
    auto BufSaida   = Buffer(D3D12_HEAP_TYPE_DEFAULT, SaidaBytes, true,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto BufLeitura = Buffer(D3D12_HEAP_TYPE_READBACK, SaidaBytes, false,
                             D3D12_RESOURCE_STATE_COPY_DEST);

    auto CpuAt = [&](UINT I) {
        auto H = Heap->GetCPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<SIZE_T>(I) * Step;
        return H;
    };
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC D{};
        D.Format                     = DXGI_FORMAT_UNKNOWN;
        D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        D.Buffer.NumElements         = kInstancias;
        D.Buffer.StructureByteStride = sizeof(FGpuSceneInstanceTransform);
        Device->CreateShaderResourceView(BufCurrent.Get(), &D, CpuAt(0));
        Device->CreateShaderResourceView(BufPrevious.Get(), &D, CpuAt(1));

        D3D12_UNORDERED_ACCESS_VIEW_DESC U{};
        U.Format                     = DXGI_FORMAT_UNKNOWN;
        U.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        U.Buffer.NumElements         = kInstancias * kFloat4sPorInstancia;
        U.Buffer.StructureByteStride = 4u * sizeof(f32);
        Device->CreateUnorderedAccessView(BufSaida.Get(), nullptr, &U, CpuAt(2));
    }

    // O CB carrega o uint4 de parametros seguido das TRES matrizes de view, na ordem do
    // GpuViewMatrices. O offset de 16 B e o proprio uint4 — se ele mudar de tamanho, a sonda le as
    // matrizes deslocadas e o teste fica vermelho, que e o desfecho certo.
    alignas(16) u8 CB[256]{};
    {
        const u32 P[4]{ kInstancias, 0u, 0u, 0u };
        std::memcpy(CB, P, sizeof(P));
        static_assert(sizeof(FGpuViewMatrices) + 16u <= sizeof(CB));
        std::memcpy(CB + 16, &View, sizeof(View));
    }
    auto BufCB = Upload(CB, sizeof(CB));

    wrl::ComPtr<ID3D12CommandQueue>        Queue;
    wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
    wrl::ComPtr<ID3D12GraphicsCommandList> List;
    wrl::ComPtr<ID3D12Fence>               Fence;
    D3D12_COMMAND_QUEUE_DESC QD{};
    Hr(Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&Queue)), "CreateCommandQueue");
    Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Allocator)),
       "CreateCommandAllocator");
    Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                 Pipeline.Get(), IID_PPV_ARGS(&List)), "CreateCommandList");
    Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence");

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
    List->SetComputeRootDescriptorTable(2, GpuAt(2));
    List->Dispatch(1, 1, 1);
    {
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = BufSaida.Get();
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        B.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        List->ResourceBarrier(1, &B);
        List->CopyResource(BufLeitura.Get(), BufSaida.Get());
    }
    Hr(List->Close(), "Close");

    ID3D12CommandList* Listas[] = { List.Get() };
    Queue->ExecuteCommandLists(1, Listas);
    Hr(Queue->Signal(Fence.Get(), 1), "Signal");
    if (Fence->GetCompletedValue() < 1) {
        struct FEvent {
            HANDLE Value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ~FEvent() { if (Value) CloseHandle(Value); }
        } Evento;
        if (!Evento.Value) Hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
        Hr(Fence->SetEventOnCompletion(1, Evento.Value), "SetEventOnCompletion");
        if (WaitForSingleObject(Evento.Value, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("a sonda nao terminou");
    }

    std::vector<f32> Saida(static_cast<size_t>(kInstancias) * kFloat4sPorInstancia * 4u);
    {
        void* P = nullptr;
        Hr(BufLeitura->Map(0, nullptr, &P), "Map(leitura)");
        std::memcpy(Saida.data(), P, static_cast<size_t>(SaidaBytes));
        BufLeitura->Unmap(0, nullptr);
    }

    // --- paridade ---------------------------------------------------------------------------------
    for (u32 I = 0; I < kInstancias; ++I) {
        const FGpuObjectTransforms Esperado =
            ComposeObjectTransforms(View, Current[I].Value, Previous[I].Value);
        const f32* Base = Saida.data() + static_cast<size_t>(I) * kFloat4sPorInstancia * 4u;

        Check(MatrizIgual(Esperado.MVP, Base + 0),
              "MVP composto no shader casa com o do C++");
        Check(MatrizIgual(Esperado.ModelMatrix, Base + 16),
              "ModelMatrix atravessa a composicao sem ser tocada");
        Check(MatrizIgual(Esperado.CurMVPNoJitter, Base + 32),
              "CurMVPNoJitter composto no shader casa com o do C++");
        Check(MatrizIgual(Esperado.PrevMVP, Base + 48),
              "PrevMVP composto no shader casa com o do C++");
    }

    // As tres matrizes de view TEM de produzir resultados diferentes entre si. Sem isto, um shader
    // que usasse ViewProj nas tres passaria em tudo acima.
    {
        const f32* Base = Saida.data();
        bool MvpIgualNoJitter = true, MvpIgualPrev = true;
        for (u32 K = 0; K < 16; ++K) {
            MvpIgualNoJitter = MvpIgualNoJitter && Perto(Base[K], Base[32 + K]);
            MvpIgualPrev     = MvpIgualPrev     && Perto(Base[K], Base[48 + K]);
        }
        Check(!MvpIgualNoJitter, "MVP e CurMVPNoJitter usam matrizes de view distintas");
        Check(!MvpIgualPrev, "MVP e PrevMVP usam matrizes de view distintas");
    }

    // O caso "sem historico": previous == current faz PrevMVP depender so da view anterior. Ele
    // prova que a coluna PrevTransform e de fato lida — um shader que usasse Current nas duas
    // colunas passaria em tudo, menos nas instancias em que as duas diferem.
    {
        const u32 I = kInstancias - 1u;
        const f32* Base = Saida.data() + static_cast<size_t>(I) * kFloat4sPorInstancia * 4u;
        const Mat44 Esperado = Current[I].Value * View.PrevViewProjUnjittered;
        Check(MatrizIgual(Esperado, Base + 48),
              "com previous == current, PrevMVP usa a view anterior sobre o transform atual");
    }

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
} catch (const std::exception& E) {
    std::cerr << "  FAIL: " << E.what() << "\n";
    return 1;
}
