// Paridade do par temporal derivado, executada em WARP (Fase 4 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir: a aposentadoria do `TransformBuffer` do TemporalMotion tira
// da CPU uma tabela de 128 B por instancia -- reescrita INTEIRA por frame, com duas inversas 4x4
// por entrada -- e passa a derivar o mesmo par na LEITURA, a partir das duas colunas canonicas que
// ja existem na tabela de instancias.
//
// O risco nao e de layout. E a INVERSA e a ORDEM DO PRODUTO: um sinal de cofator trocado, ou
// `Previous * Current.Inverse()` no lugar de `Current.Inverse() * Previous`, compila, executa e
// reprojeta para o lugar errado -- so em movimento, que e onde o suspeito natural seria o denoiser.
//
// Os dois lados sao os de PRODUCAO: `Smile::ComposeTemporalInstanceTransform`, que o UpdatePerFrame
// chama, e `TemporalComposeInstanceTransform` do TemporalMotionCommon.hlsli, que os consumidores
// vao chamar. Nenhum dos dois e reescrito aqui.
//
// WARP nao exige placa. O que ele NAO cobre e o efeito visual da troca nos tres consumidores, que
// fica para o A/B em GPU.

#include <cmath>
#include <cstdio>
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

#include "Smile/Graphics/Scene/TemporalInstanceTransform.h"
#include "Smile/Graphics/Scene/GpuSceneTables.h"
#include "Smile/Scene/Scene.h"

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

    // Tolerancia RELATIVA. A CPU e a WARP nao precisam contrair os mesmos produtos em FMA, e uma
    // inversa acumula dezesseis cofatores antes de dividir -- um epsilon absoluto reprovaria
    // translacao grande enquanto aceitaria erro grosseiro perto da origem.
    //
    // 1e-4 relativo, e nao 1e-5 como na sonda de composicao: aqui ha uma DIVISAO por determinante
    // no meio, e a razao entre o maior e o menor termo do cofator cresce com a escala nao uniforme.
    // Ainda separa arredondamento de troca de ordem por varias ordens de grandeza -- uma inversa
    // errada erra o valor inteiro, nao o quarto digito.
    bool Perto(f32 _A, f32 _B) {
        const f32 Escala = std::max({ 1.0f, std::fabs(_A), std::fabs(_B) });
        return std::fabs(_A - _B) <= 1e-4f * Escala;
    }

    bool MatrizIgual(const Mat44& _A, const f32* _B) {
        for (u32 R = 0; R < 4; ++R)
            for (u32 C = 0; C < 4; ++C)
                if (!Perto(_A.M[R][C], _B[R * 4u + C])) return false;
        return true;
    }

    // TRS de verdade, e nao uma matriz densa arbitraria: a `Densa` da sonda de composicao tem
    // linhas em progressao aritmetica, que e RANK 2 -- determinante zero, e toda instancia cairia
    // no ramo singular. Um teste de inversa feito com ela provaria so o ramo de excecao.
    //
    // Rotacao nos TRES eixos, e nao so em Y. Uma rotacao de eixo unico deixa ZEROS estruturais no
    // 3x3 (com RotationY, M[0][1] e M[2][1] sao zero), e cada zero apaga termos inteiros do
    // cofator: um sinal trocado num deles multiplica zero e o teste fica verde. Medido -- inverter
    // o sinal do primeiro termo de `inv[13]` passava com rotacao so em Y.
    Mat44 Trs(const Vec3& _T, const Vec3& _Euler, const Vec3& _S) {
        return Mat44::Scale(_S) * Mat44::RotationEulerXYZ(_Euler) * Mat44::Translation(_T);
    }

    // Perturba a ultima COLUNA, que num TRS e sempre (0,0,0,1). Nenhum transform de objeto e assim,
    // e e justamente por isso: metade dos termos da formula de cofator multiplica m[3], m[7] ou
    // m[11], e com entrada afim eles sao zero -- estruturalmente inalcancaveis. A funcao sob teste e
    // uma inversa 4x4 GERAL, e o que se prova aqui e a TRANSCRICAO dela, nao o uso.
    Mat44 ComProjecao(const Mat44& _M, f32 _A, f32 _B, f32 _C) {
        Mat44 R = _M;
        R.M[0][3] = _A;
        R.M[1][3] = _B;
        R.M[2][3] = _C;
        return R;
    }

    constexpr u32 kInstancias = 6;
    // O par (2 matrizes) mais os dois sentidos calculados separadamente (2) = 4 matrizes.
    constexpr u32 kFloat4sPorInstancia = 16u;
}

int main() try {
    std::cout << "Smile.TemporalTransformWarp\n";

    const std::vector<u8> ProductionSource = LerArquivo(SMILE_TEMPORAL_MOTION_CPP_PATH);
    const std::string ProductionText(ProductionSource.begin(), ProductionSource.end());
    Check(!ProductionText.empty(), "fonte do passe temporal foi encontrado");
    Check(ProductionText.find("MappedTransforms") == std::string::npos,
          "o upload mapeado de transforms aposentado nao voltou");
    Check(ProductionText.find("FTemporalInstanceTransformGPU") == std::string::npos,
          "a tabela derivada por instancia aposentada nao voltou");

    const std::vector<u8> Bytecode = LerArquivo(SMILE_TEMPORAL_TRANSFORM_PROBE_CSO);
    Check(!Bytecode.empty(), "a sonda compilada foi encontrada");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_TEMPORAL_TRANSFORM_PROBE_CSO << ")\n";
        return 1;
    }

    // --- dados ------------------------------------------------------------------------------
    std::vector<FGpuSceneInstanceTransform> Current(kInstancias), Previous(kInstancias);

    // 0: o caso comum -- translacao, rotacao e escala NAO uniforme, que se moveu entre os frames.
    Current[0].Value  = Trs({  3.5f, -1.25f, 7.75f }, { 0.41f, 0.62f, -0.27f }, { 1.7f, 0.6f, 1.15f });
    Previous[0].Value = Trs({  3.1f, -1.10f, 7.20f }, { 0.35f, 0.48f, -0.19f }, { 1.7f, 0.6f, 1.15f });

    // 1: ESPELHADO (escala negativa num eixo). Determinante negativo -- o sinal tem de atravessar
    //    a inversa inteira, e um cofator trocado aparece aqui antes de aparecer em qualquer outro.
    Current[1].Value  = Trs({ -12.0f, 4.0f, -3.0f }, { -0.88f, 1.91f, 0.53f }, { -2.2f, 1.4f, 0.8f });
    Previous[1].Value = Trs({ -11.4f, 4.3f, -3.6f }, { -0.79f, 2.05f, 0.61f }, { -2.2f, 1.4f, 0.8f });

    // 2: longe da origem, com escala grande. Se a comparacao fosse por epsilon absoluto, seria
    //    este que reprovaria sem haver defeito.
    Current[2].Value  = Trs({ 4200.0f, -880.0f, 15000.0f }, { 0.07f, 0.13f, 0.29f }, { 9.0f, 9.0f, 9.0f });
    Previous[2].Value = Trs({ 4197.5f, -880.5f, 14996.0f }, { 0.05f, 0.11f, 0.24f }, { 9.0f, 9.0f, 9.0f });

    // 3: SEM HISTORICO -- previous igual ao current. Os dois derivados tem de dar IDENTIDADE, e e
    //    o unico caso do conjunto em que se sabe a resposta exata sem calcular nada.
    Current[3].Value  = Trs({ 0.75f, 2.5f, -4.25f }, { 1.10f, 2.75f, -0.62f }, { 1.3f, 1.3f, 1.3f });
    Previous[3].Value = Current[3].Value;

    // 4: SINGULAR (escala zero num eixo). O C++ devolve identidade da inversa, entao o par vira
    //    {Previous, Current} -- um resultado esquisito e DEFINIDO. O que nao pode e divergir entre
    //    os dois lados, nem virar NaN: um NaN daqui contamina a reprojecao do frame inteiro.
    Current[4].Value  = Trs({ 1.0f, 1.0f, 1.0f }, { 0.21f, 0.33f, 0.47f }, { 2.0f, 0.0f, 1.0f });
    Previous[4].Value = Trs({ 1.2f, 0.9f, 1.4f }, { 0.26f, 0.41f, 0.52f }, { 2.0f, 0.0f, 1.0f });

    // 5: NAO AFIM. Nenhum objeto tem transform assim -- e o ponto. Com a ultima coluna em (0,0,0,1),
    //    metade dos termos de cada cofator multiplica zero, e um sinal trocado neles fica invisivel
    //    por construcao. Este caso e o unico que alcanca a formula inteira.
    Current[5].Value  = ComProjecao(Current[0].Value,   0.031f, -0.019f, 0.047f);
    Previous[5].Value = ComProjecao(Previous[0].Value, -0.024f,  0.036f, 0.012f);

    // --- WARP -------------------------------------------------------------------------------
    wrl::ComPtr<ID3D12Device> Device;
    {
        wrl::ComPtr<IDXGIFactory4> Factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1");
        wrl::ComPtr<IDXGIAdapter> Warp;
        Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter");
        Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)),
           "D3D12CreateDevice");
    }

    // Memory gate for the retired resource. Query the allocation footprint on the same D3D12
    // device without creating the buffer: the final production source guard above proves that
    // this allocation no longer exists, while this query records what was removed at the exact
    // capacity used by the 1M scene.
    {
        constexpr u32 kOneMillion = 1'000'000u;
        constexpr u64 kLegacyBytes =
            2ull * SceneCapacityFor(kOneMillion) * sizeof(FTemporalInstanceTransformGPU);
        D3D12_RESOURCE_DESC Legacy{};
        Legacy.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        Legacy.Width = kLegacyBytes;
        Legacy.Height = 1;
        Legacy.DepthOrArraySize = 1;
        Legacy.MipLevels = 1;
        Legacy.SampleDesc.Count = 1;
        Legacy.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_RESOURCE_ALLOCATION_INFO Info =
            Device->GetResourceAllocationInfo(0, 1, &Legacy);
        Check(Info.SizeInBytes >= kLegacyBytes,
              "footprint D3D12 do TransformBuffer classico cobre os bytes pedidos");
        std::cout << "  legacy TransformBuffer @1M: " << Info.SizeInBytes
                  << " bytes reserved\n";
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

    alignas(16) u8 CB[256]{};
    {
        const u32 P[4]{ kInstancias, 0u, 0u, 0u };
        std::memcpy(CB, P, sizeof(P));
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

    auto Base = [&](u32 I) {
        return Saida.data() + static_cast<size_t>(I) * kFloat4sPorInstancia * 4u;
    };

    // --- paridade ---------------------------------------------------------------------------
    for (u32 I = 0; I < kInstancias; ++I) {
        const FTemporalInstanceTransformGPU Esperado =
            ComposeTemporalInstanceTransform(Current[I].Value, Previous[I].Value);
        Check(MatrizIgual(Esperado.CurrentToPrevious, Base(I) + 0),
              "CurrentToPrevious derivado no shader casa com o do C++");
        Check(MatrizIgual(Esperado.PreviousToCurrent, Base(I) + 16),
              "PreviousToCurrent derivado no shader casa com o do C++");

        // Os helpers de UM sentido sao o que os consumidores vao chamar -- chamar o par e
        // descartar metade custaria uma inversa a mais por leitura. Eles tem de dar EXATAMENTE o
        // mesmo que a metade correspondente do par, senao a paridade acima cobre uma funcao e a
        // producao usa outra. Igualdade de bits: e a mesma expressao, no mesmo shader.
        Check(std::memcmp(Base(I) + 0,  Base(I) + 32, 16 * sizeof(f32)) == 0,
              "TemporalCurrentToPrevious e a metade de ida do par, bit a bit");
        Check(std::memcmp(Base(I) + 16, Base(I) + 48, 16 * sizeof(f32)) == 0,
              "TemporalPreviousToCurrent e a metade de volta do par, bit a bit");
    }

    // As duas matrizes NAO podem ser iguais entre si nos casos que se moveram. Sem isto, um shader
    // que devolvesse a mesma nas duas -- ou que invertesse a coluna errada -- passaria em tudo
    // acima nos casos 3 e 4 e seria pego so por sorte nos outros.
    for (const u32 I : { 0u, 1u, 2u, 5u }) {
        bool Iguais = true;
        for (u32 K = 0; K < 16 && Iguais; ++K) Iguais = Perto(Base(I)[K], Base(I)[16 + K]);
        Check(!Iguais, "os dois sentidos da reprojecao sao matrizes distintas");
    }

    // ROUND-TRIP, so com a saida do shader: (C->P) * (P->C) = C-1 P P-1 C = I. Esta assercao nao
    // depende de o C++ estar certo -- se as duas implementacoes errassem a inversa DA MESMA FORMA,
    // a paridade acima ficaria verde e esta aqui nao, porque uma inversa errada nao se cancela.
    //
    // Fora dos singulares (4), onde a identidade do corte de determinante quebra a propriedade de
    // proposito: la o par vira {Previous, Current} e o produto nao e identidade.
    for (const u32 I : { 0u, 1u, 2u, 3u, 5u }) {
        Mat44 A{}, B{};
        std::memcpy(&A, Base(I) + 0,  sizeof(Mat44));
        std::memcpy(&B, Base(I) + 16, sizeof(Mat44));
        const Mat44 Produto = A * B;
        // Tolerancia PROPRIA: o produto acumula o erro das duas inversas mais o da multiplicacao,
        // e o caso 2 tem coordenadas na casa dos 15.000 com escala 9 -- a translacao da inversa
        // carrega a razao entre eles. 3e-3 relativo ainda reprova qualquer inversa errada, que
        // erra por ordens de grandeza e nao pelo terceiro digito.
        for (u32 R = 0; R < 4; ++R)
            for (u32 C = 0; C < 4; ++C) {
                const f32 Esperado = (R == C) ? 1.0f : 0.0f;
                const f32 Escala   = std::max({ 1.0f, std::fabs(Produto.M[R][C]) });
                Check(std::fabs(Produto.M[R][C] - Esperado) <= 3e-3f * Escala,
                      "o par derivado faz round-trip para a identidade");
            }
    }

    // O caso SEM HISTORICO tem resposta exata: previous == current faz os dois virarem IDENTIDADE.
    // E o unico ponto do teste que nao depende da referencia do C++ estar certa -- se as duas
    // implementacoes errassem JUNTAS, e aqui que apareceria.
    {
        const Mat44 Id = Mat44::Identity();
        Check(MatrizIgual(Id, Base(3) + 0),
              "sem historico, CurrentToPrevious e a identidade");
        Check(MatrizIgual(Id, Base(3) + 16),
              "sem historico, PreviousToCurrent e a identidade");
    }

    // SINGULAR: o corte de determinante devolve identidade da inversa nos dois lados, entao o par
    // vira {Previous, Current}. O que se prova aqui e que o ramo existe no shader -- sem ele o
    // resultado seria NaN, e NaN passa despercebido ate contaminar o historico inteiro.
    {
        Check(MatrizIgual(Previous[4].Value, Base(4) + 0),
              "matriz singular cai no ramo de identidade, como no C++");
        for (u32 K = 0; K < 32; ++K)
            Check(std::isfinite(Base(4)[K]), "a inversa singular nao produziu NaN/Inf");
    }

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
} catch (const std::exception& E) {
    std::cerr << "  FAIL: " << E.what() << "\n";
    return 1;
}
