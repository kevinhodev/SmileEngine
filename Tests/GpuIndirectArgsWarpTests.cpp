// Contrato do comando indireto do main view, executado em WARP (Fase 5 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir: o layout de `FGpuIndirectDrawArgs` NAO e escolhido pela
// struct. Ele e ditado pela ORDEM das entradas em `pArgumentDescs`, e o leitor do outro lado e o
// COMMAND PROCESSOR, nao um shader — entao nao ha compilador que valide nada. Com as duas entradas
// de constante trocadas, tudo compila, o comando executa, e a linha do material chega no slot da
// instancia.
//
// Por isso o teste EXECUTA o comando. Um compute escrever o buffer e a CPU rele-lo provaria so o
// produtor: os dois lados concordariam entre si e estariam os dois errados sobre o que o D3D12 vai
// ler. Aqui o caminho inteiro roda — compute monta pela struct HLSL, `ExecuteIndirect` consome pela
// command signature de PRODUCAO, e um vertex shader relata o que recebeu.
//
// O endereco de escrita do observador e o proprio `SV_VertexID`, que num draw indexado e o valor
// lido do index buffer. Entao um `StartIndexLocation` errado nao produz valor errado: produz escrita
// em OUTRO lugar, e o teste confere o CONJUNTO de posicoes escritas, nao so o conteudo delas.
//
// `BaseVertexLocation` NAO chega no SV_VertexID — medido em WARP. Quem o prova e o atributo buscado
// do vertex buffer, onde `VB[k] == k`: o fetch e `VB[IndexBuffer[i] + BaseVertexLocation]`, entao a
// diferenca para o SV_VertexID E o campo. Sem esse vertex buffer o campo seria inobservavel, e o
// teste estaria declarando uma cobertura que nao tem.
//
// ⚠️ A topologia e TRIANGLELIST e `IndexCountPerInstance` tem de ser multiplo de 3: o resto de uma
// primitiva incompleta e descartado em silencio. A primeira versao deste teste usava 2 e 4 indices,
// e os comandos com 2 nao produziam vertice nenhum — o sintoma apontava para a command signature,
// que nao tinha nada a ver com isso.
//
// WARP nao exige placa. O que ele nao cobre e o comportamento sob as PSOs de producao, que fica
// para o A/B da slice que trocar o passe.

#include <algorithm>
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

#include "Smile/Graphics/Scene/GpuIndirectDraw.h"

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

    // Indice do bloco de constantes na root signature do TESTE. Nao e 0 de proposito: se a
    // descricao compartilhada passasse a cravar `RootParameterIndex = 0`, o parametro 0 daqui e uma
    // tabela de UAV e o `CreateCommandSignature` recusaria. O indice de PRODUCAO (14) nao e
    // testavel daqui, e nem precisa ser: ele e o mesmo `kMaterialRootIndex` que todo
    // `SetGraphicsRoot32BitConstant` do raster ja usa.
    constexpr u32 kTestDrawConstantsRootIndex = 1;

    // ESCRITOS 4, EXECUTADOS 2. O `MaxCommandCount` do ExecuteIndirect e validado contra o TAMANHO
    // do buffer, e nao contra o count buffer -- a camada de debug recusa `stride * max + offset`
    // maior que o recurso. Entao o buffer e dimensionado pelo maximo, e quem limita a execucao e o
    // count buffer. Os dois ultimos comandos existem justamente para PROVAR isso: eles sao validos
    // e escreveriam em posicoes proprias se fossem executados.
    constexpr u32 kCommandCount   = 4;
    constexpr u32 kExecutedCount  = 2;
    constexpr u32 kIndexCount     = 64;   // valores sequenciais: IB[i] == i
    constexpr u32 kVertexCount    = 1024; // valores sequenciais: VB[k] == k
    constexpr u32 kObservedSlots  = 1024; // dois uint4 por vertice
    constexpr u32 kSentinela      = 0xFFFFFFFFu;

    // Veneno nas DUAS constantes que o comando escreve. Sem isto, uma command signature que nao
    // escrevesse constante nenhuma poderia passar por acidente: o valor anterior seria lido como se
    // fosse o do comando. Com veneno, "nao escrito" e distinguivel de "escrito certo".
    constexpr u32 kVenenoMaterialRow  = 0xDEAD0001u;
    constexpr u32 kVenenoInstanceSlot = 0xDEAD0002u;
    // Os tres que o PASSE amarra e que tem de SOBREVIVER ao ExecuteIndirect.
    constexpr u32 kPasseMaterialTable  = 0x000000A1u;
    constexpr u32 kPasseTransformSrv   = 0x000000B2u;
    constexpr u32 kPassePrevTransform  = 0x000000C3u;

    struct FComando {
        u32 MaterialRow;
        u32 InstanceSlot;
        u32 IndexCountPerInstance;
        u32 StartIndexLocation;
        i32 BaseVertexLocation;
    };

    // Valores TODOS distintos entre si e entre campos: com 1 e 0 espalhados, trocar dois campos
    // continuaria batendo em metade das comparacoes.
    //
    // ⚠️ `IndexCountPerInstance` MULTIPLO DE 3. A topologia e TRIANGLELIST, e o resto de uma
    // primitiva incompleta e descartado em silencio: a primeira versao deste teste usava 2 e 4, e os
    // comandos com 2 indices simplesmente nao produziam vertice nenhum. O sintoma — "o comando nao
    // executou" — aponta para a command signature, que nao tinha nada a ver com isso.
    //
    // Faixas de indice DISJUNTAS: se dois comandos escrevessem nos mesmos vertices, o ultimo
    // venceria e o teste leria um comando no lugar do outro.
    constexpr FComando kComandos[kCommandCount] = {
        { 3u,  7u,  3u,  0u, 100 },
        { 11u, 41u, 6u,  6u, 200 },
        // Alem do count: validos, e tem de NAO executar.
        { 23u, 71u, 3u, 24u, 300 },
        { 29u, 83u, 3u, 30u, 400 },
    };
}

int main() try {
    std::cout << "Smile.GpuIndirectArgsWarp\n";

    const std::vector<u8> ComputeBytecode = LerArquivo(SMILE_INDIRECT_ARGS_PROBE_CS_CSO);
    const std::vector<u8> VertexBytecode  = LerArquivo(SMILE_INDIRECT_ARGS_PROBE_VS_CSO);
    Check(!ComputeBytecode.empty(), "a sonda de compute compilada foi encontrada");
    Check(!VertexBytecode.empty(), "a sonda de vertex compilada foi encontrada");
    if (ComputeBytecode.empty() || VertexBytecode.empty()) return 1;

    // --- WARP -------------------------------------------------------------------------------
    //
    // Com DEBUG LAYER, e nao por zelo: o erro deste caminho e um HRESULT no `Close`, sem nenhuma
    // pista de qual das dezenas de coisas gravadas estava errada. A camada e o unico jeito de a
    // falha dizer o que aconteceu, e num teste em WARP o custo nao importa.
    wrl::ComPtr<ID3D12Debug> DebugLayer;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugLayer)))) DebugLayer->EnableDebugLayer();

    wrl::ComPtr<ID3D12Device> Device;
    {
        wrl::ComPtr<IDXGIFactory4> Factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1");
        wrl::ComPtr<IDXGIAdapter> Warp;
        Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter");
        Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)),
           "D3D12CreateDevice");
    }

    // Despeja o que a camada tiver a dizer. Chamado no fim e tambem quando algo falha.
    wrl::ComPtr<ID3D12InfoQueue> InfoQueue;
    Device.As(&InfoQueue);
    auto DespejarMensagens = [&]() {
        if (!InfoQueue) return;
        const UINT64 N = InfoQueue->GetNumStoredMessages();
        for (UINT64 I = 0; I < N; ++I) {
            SIZE_T Bytes = 0;
            if (FAILED(InfoQueue->GetMessage(I, nullptr, &Bytes))) continue;
            std::vector<u8> Buf(Bytes);
            auto* Msg = reinterpret_cast<D3D12_MESSAGE*>(Buf.data());
            if (FAILED(InfoQueue->GetMessage(I, Msg, &Bytes))) continue;
            if (Msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                std::cerr << "  [D3D12] " << Msg->pDescription << "\n";
        }
        InfoQueue->ClearStoredMessages();
    };

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

    // --- entradas ---------------------------------------------------------------------------
    // Dois uint4 por comando, num layout que NAO compartilha nada com o de saida: se a sonda
    // copiasse de uma StructuredBuffer<GpuIndirectDrawArgs>, o compilador viraria memcpy e um
    // espelho com campos trocados produziria os mesmos bytes.
    std::vector<u32> Specs(kCommandCount * 8u, 0u);
    for (u32 I = 0; I < kCommandCount; ++I) {
        const FComando& C = kComandos[I];
        Specs[I * 8u + 0u] = C.MaterialRow;
        Specs[I * 8u + 1u] = C.InstanceSlot;
        Specs[I * 8u + 2u] = C.IndexCountPerInstance;
        Specs[I * 8u + 3u] = 1u; // InstanceCount
        Specs[I * 8u + 4u] = C.StartIndexLocation;
        std::memcpy(&Specs[I * 8u + 5u], &C.BaseVertexLocation, sizeof(i32));
        Specs[I * 8u + 6u] = 0u; // StartInstanceLocation
        Specs[I * 8u + 7u] = 0u;
    }
    auto BufSpecs = Upload(Specs.data(), Specs.size() * sizeof(u32));

    std::vector<u32> Indices(kIndexCount);
    for (u32 I = 0; I < kIndexCount; ++I) Indices[I] = I;
    auto BufIndices = Upload(Indices.data(), Indices.size() * sizeof(u32));

    // `VB[k] == k`. E o que torna `BaseVertexLocation` OBSERVAVEL: o fetch e
    // `VB[IndexBuffer[i] + BaseVertexLocation]`, entao o valor que o shader recebe menos o
    // SV_VertexID e exatamente o campo. Sem vertex buffer o campo nao teria efeito visivel nenhum,
    // e o teste estaria declarando uma cobertura que nao tem.
    std::vector<u32> Vertices(kVertexCount);
    for (u32 I = 0; I < kVertexCount; ++I) Vertices[I] = I;
    auto BufVertices = Upload(Vertices.data(), Vertices.size() * sizeof(u32));

    const u32 Contagem = kExecutedCount;
    auto BufCount = Upload(&Contagem, sizeof(u32));

    const u64 ArgsBytes = static_cast<u64>(kCommandCount) * sizeof(FGpuIndirectDrawArgs);
    // COMMON na criacao: buffers sao criados em COMMON de qualquer jeito, e pedir outra coisa so
    // rende aviso da camada de debug. A promocao automatica cobre o primeiro uso.
    auto BufArgs = Buffer(D3D12_HEAP_TYPE_DEFAULT, ArgsBytes, true,
                          D3D12_RESOURCE_STATE_COMMON);

    const u64 ObservedBytes = static_cast<u64>(kObservedSlots) * 4u * sizeof(u32);
    auto BufObserved = Buffer(D3D12_HEAP_TYPE_DEFAULT, ObservedBytes, true,
                              D3D12_RESOURCE_STATE_COMMON);
    auto BufLeitura  = Buffer(D3D12_HEAP_TYPE_READBACK, ObservedBytes, false,
                              D3D12_RESOURCE_STATE_COPY_DEST);
    // Sentinela por copia, e nao por ClearUAV: o clear exigiria um segundo heap nao visivel ao
    // shader so para isto. O que importa e distinguir "escrito" de "nao escrito".
    std::vector<u32> Sentinelas(kObservedSlots * 4u, kSentinela);
    auto BufSentinela = Upload(Sentinelas.data(), ObservedBytes);

    alignas(16) u8 CB[256]{};
    { const u32 P[4]{ kCommandCount, 0u, 0u, 0u }; std::memcpy(CB, P, sizeof(P)); }
    auto BufCB = Upload(CB, sizeof(CB));

    // --- descritores ------------------------------------------------------------------------
    wrl::ComPtr<ID3D12DescriptorHeap> Heap;
    D3D12_DESCRIPTOR_HEAP_DESC HD{};
    HD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HD.NumDescriptors = 3;
    HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Hr(Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&Heap)), "CreateDescriptorHeap");
    const UINT Step = Device->GetDescriptorHandleIncrementSize(HD.Type);
    auto CpuAt = [&](UINT I) {
        auto H = Heap->GetCPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<SIZE_T>(I) * Step;
        return H;
    };
    auto GpuAt = [&](UINT I) {
        auto H = Heap->GetGPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<UINT64>(I) * Step;
        return H;
    };
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC U{};
        U.Format                     = DXGI_FORMAT_UNKNOWN;
        U.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        U.Buffer.NumElements         = kCommandCount;
        U.Buffer.StructureByteStride = sizeof(FGpuIndirectDrawArgs);
        Device->CreateUnorderedAccessView(BufArgs.Get(), nullptr, &U, CpuAt(0));

        D3D12_SHADER_RESOURCE_VIEW_DESC S{};
        S.Format                     = DXGI_FORMAT_UNKNOWN;
        S.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        S.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        S.Buffer.NumElements         = kCommandCount * 2u;
        S.Buffer.StructureByteStride = 4u * sizeof(u32);
        Device->CreateShaderResourceView(BufSpecs.Get(), &S, CpuAt(1));

        D3D12_UNORDERED_ACCESS_VIEW_DESC O{};
        O.Format                     = DXGI_FORMAT_UNKNOWN;
        O.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        O.Buffer.NumElements         = kObservedSlots;
        O.Buffer.StructureByteStride = 4u * sizeof(u32);
        Device->CreateUnorderedAccessView(BufObserved.Get(), nullptr, &O, CpuAt(2));
    }

    // --- root signatures --------------------------------------------------------------------
    auto Serializar = [&](const D3D12_ROOT_SIGNATURE_DESC& _Desc, const char* _Onde) {
        wrl::ComPtr<ID3DBlob> Serial, Erro;
        const HRESULT H = D3D12SerializeRootSignature(&_Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                      &Serial, &Erro);
        if (FAILED(H) && Erro)
            std::cerr << "  " << static_cast<const char*>(Erro->GetBufferPointer()) << "\n";
        Hr(H, _Onde);
        wrl::ComPtr<ID3D12RootSignature> Root;
        Hr(Device->CreateRootSignature(0, Serial->GetBufferPointer(), Serial->GetBufferSize(),
                                       IID_PPV_ARGS(&Root)), _Onde);
        return Root;
    };

    wrl::ComPtr<ID3D12RootSignature> ComputeRoot;
    {
        D3D12_DESCRIPTOR_RANGE Ranges[2]{};
        Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        Ranges[0].NumDescriptors = 1;
        Ranges[1].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        Ranges[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER P[3]{};
        P[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (int I = 0; I < 2; ++I) {
            P[I + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            P[I + 1].DescriptorTable.NumDescriptorRanges = 1;
            P[I + 1].DescriptorTable.pDescriptorRanges   = &Ranges[I];
        }
        D3D12_ROOT_SIGNATURE_DESC D{};
        D.NumParameters = 3;
        D.pParameters   = P;
        ComputeRoot = Serializar(D, "root signature de compute");
    }

    wrl::ComPtr<ID3D12RootSignature> GraphicsRoot;
    {
        D3D12_DESCRIPTOR_RANGE Range{};
        Range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        Range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER P[2]{};
        P[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        P[0].DescriptorTable.NumDescriptorRanges = 1;
        P[0].DescriptorTable.pDescriptorRanges   = &Range;
        P[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // O bloco de PRODUCAO: cinco constantes em b4, visibilidade ALL. A contagem vem do
        // contrato, nao de um 5 solto — se o bloco crescer, isto cresce junto.
        P[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        P[1].Constants.ShaderRegister = 4;
        P[1].Constants.Num32BitValues = GpuDrawConstants::kCount;
        P[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        static_assert(kTestDrawConstantsRootIndex == 1, "o indice tem de casar com P[1]");
        D3D12_ROOT_SIGNATURE_DESC D{};
        D.NumParameters = 2;
        D.pParameters   = P;
        D.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        GraphicsRoot = Serializar(D, "root signature de graphics");
    }

    // --- command signature: a de PRODUCAO, pela mesma funcao ---------------------------------
    wrl::ComPtr<ID3D12CommandSignature> CommandSignature;
    {
        D3D12_INDIRECT_ARGUMENT_DESC ArgumentDescs[3]{};
        const D3D12_COMMAND_SIGNATURE_DESC Desc =
            MakeIndirectDrawSignatureDesc(ArgumentDescs, kTestDrawConstantsRootIndex);
        Check(Desc.ByteStride == sizeof(FGpuIndirectDrawArgs),
              "o stride declarado e o tamanho da struct");
        Hr(Device->CreateCommandSignature(&Desc, GraphicsRoot.Get(),
                                          IID_PPV_ARGS(&CommandSignature)),
           "CreateCommandSignature");
    }

    // --- pipelines --------------------------------------------------------------------------
    wrl::ComPtr<ID3D12PipelineState> ComputePSO;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC D{};
        D.pRootSignature = ComputeRoot.Get();
        D.CS             = { ComputeBytecode.data(), ComputeBytecode.size() };
        Hr(Device->CreateComputePipelineState(&D, IID_PPV_ARGS(&ComputePSO)),
           "CreateComputePipelineState");
    }

    wrl::ComPtr<ID3D12PipelineState> GraphicsPSO;
    {
        // Sem pixel shader e sem render target: a sonda so existe pelo efeito colateral do UAV no
        // vertex shader, e rasterizar nao acrescenta nada ao que ela mede.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC D{};
        D.pRootSignature        = GraphicsRoot.Get();
        D.VS                    = { VertexBytecode.data(), VertexBytecode.size() };
        D.SampleMask            = UINT_MAX;
        D.SampleDesc.Count      = 1;
        D.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // Um atributo por vertice, so para tornar `BaseVertexLocation` observavel.
        D3D12_INPUT_ELEMENT_DESC Elemento{};
        Elemento.SemanticName      = "TEXCOORD";
        Elemento.Format            = DXGI_FORMAT_R32_UINT;
        Elemento.InputSlotClass    = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        D.InputLayout.pInputElementDescs = &Elemento;
        D.InputLayout.NumElements        = 1;
        D.NumRenderTargets      = 0;
        D.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        D.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        D.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        D.RasterizerState.DepthClipEnable = TRUE;
        D.DepthStencilState.DepthEnable   = FALSE;
        D.DepthStencilState.StencilEnable = FALSE;
        Hr(Device->CreateGraphicsPipelineState(&D, IID_PPV_ARGS(&GraphicsPSO)),
           "CreateGraphicsPipelineState");
    }

    // --- gravacao ---------------------------------------------------------------------------
    wrl::ComPtr<ID3D12CommandQueue>        Queue;
    wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
    wrl::ComPtr<ID3D12GraphicsCommandList> List;
    wrl::ComPtr<ID3D12Fence>               Fence;
    D3D12_COMMAND_QUEUE_DESC QD{};
    Hr(Device->CreateCommandQueue(&QD, IID_PPV_ARGS(&Queue)), "CreateCommandQueue");
    Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Allocator)),
       "CreateCommandAllocator");
    Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                 ComputePSO.Get(), IID_PPV_ARGS(&List)), "CreateCommandList");
    Hr(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), "CreateFence");

    auto Transicao = [&](ID3D12Resource* _R, D3D12_RESOURCE_STATES _De,
                         D3D12_RESOURCE_STATES _Para) {
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = _R;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = _De;
        B.Transition.StateAfter  = _Para;
        List->ResourceBarrier(1, &B);
    };

    List->CopyResource(BufObserved.Get(), BufSentinela.Get());
    Transicao(BufObserved.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // (a copia acima promoveu de COMMON para COPY_DEST; a barreira sai de la)

    ID3D12DescriptorHeap* Heaps[] = { Heap.Get() };
    List->SetDescriptorHeaps(1, Heaps);
    List->SetComputeRootSignature(ComputeRoot.Get());
    List->SetComputeRootConstantBufferView(0, BufCB->GetGPUVirtualAddress());
    List->SetComputeRootDescriptorTable(1, GpuAt(1));
    List->SetComputeRootDescriptorTable(2, GpuAt(0));
    List->Dispatch(1, 1, 1);

    Transicao(BufArgs.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    List->SetPipelineState(GraphicsPSO.Get());
    List->SetGraphicsRootSignature(GraphicsRoot.Get());
    List->SetGraphicsRootDescriptorTable(0, GpuAt(2));
    // As cinco constantes ANTES do ExecuteIndirect, com as duas do comando ENVENENADAS. Sem o
    // veneno, uma command signature que nao escrevesse constante nenhuma poderia passar por
    // acidente — o valor de antes seria lido como se fosse o do comando.
    {
        u32 Bloco[GpuDrawConstants::kCount]{};
        Bloco[GpuDrawConstants::kMaterialTable]  = kPasseMaterialTable;
        Bloco[GpuDrawConstants::kMaterialRow]    = kVenenoMaterialRow;
        Bloco[GpuDrawConstants::kTransformTable] = kPasseTransformSrv;
        Bloco[GpuDrawConstants::kPrevTransform]  = kPassePrevTransform;
        Bloco[GpuDrawConstants::kInstanceSlot]   = kVenenoInstanceSlot;
        List->SetGraphicsRoot32BitConstants(kTestDrawConstantsRootIndex, GpuDrawConstants::kCount,
                                            Bloco, 0);
    }
    D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f };
    D3D12_RECT     Scissor{ 0, 0, 16, 16 };
    List->RSSetViewports(1, &Viewport);
    List->RSSetScissorRects(1, &Scissor);
    List->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW IBV{};
    IBV.BufferLocation = BufIndices->GetGPUVirtualAddress();
    IBV.SizeInBytes    = kIndexCount * static_cast<u32>(sizeof(u32));
    IBV.Format         = DXGI_FORMAT_R32_UINT;
    List->IASetIndexBuffer(&IBV);
    D3D12_VERTEX_BUFFER_VIEW VBV{};
    VBV.BufferLocation = BufVertices->GetGPUVirtualAddress();
    VBV.SizeInBytes    = kVertexCount * static_cast<u32>(sizeof(u32));
    VBV.StrideInBytes  = static_cast<u32>(sizeof(u32));
    List->IASetVertexBuffers(0, 1, &VBV);
    // MaxCommandCount e a CAPACIDADE (4); quem limita a execucao e o count buffer (2), que e como o
    // frame vai usar isto. Os comandos 2 e 3 estao escritos e validos: se o count fosse ignorado,
    // eles escreveriam nas posicoes 300+ e 400+, e o teste ve isso.
    List->ExecuteIndirect(CommandSignature.Get(), kCommandCount, BufArgs.Get(), 0,
                          BufCount.Get(), 0);

    Transicao(BufObserved.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
    List->CopyResource(BufLeitura.Get(), BufObserved.Get());
    const HRESULT CloseHr = List->Close();
    DespejarMensagens();
    Hr(CloseHr, "Close");

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

    std::vector<u32> Observado(static_cast<size_t>(kObservedSlots) * 4u);
    {
        void* P = nullptr;
        Hr(BufLeitura->Map(0, nullptr, &P), "Map(leitura)");
        std::memcpy(Observado.data(), P, static_cast<size_t>(ObservedBytes));
        BufLeitura->Unmap(0, nullptr);
    }

    // --- verificacao --------------------------------------------------------------------------
    auto Vertice = [&](u32 _Vid) { return Observado.data() + static_cast<size_t>(_Vid) * 8u; };
    auto Escrito = [&](u32 _Vid) { return Vertice(_Vid)[3] == 1u; };

    // O CONJUNTO de posicoes escritas prova `StartIndexLocation` e `IndexCountPerInstance`: o IB e
    // sequencial, entao o SV_VertexID de cada invocacao e o proprio indice lido. `BaseVertexLocation`
    // NAO entra aqui — medido em WARP, ele nao chega no SV_VertexID; quem o prova e o atributo
    // buscado do vertex buffer, abaixo.
    std::vector<u32> Esperados;
    for (u32 C = 0; C < kExecutedCount; ++C)
        for (u32 I = 0; I < kComandos[C].IndexCountPerInstance; ++I)
            Esperados.push_back(kComandos[C].StartIndexLocation + I);

    for (u32 Vid = 0; Vid < kObservedSlots / 2u; ++Vid) {
        const bool DeviaEscrever =
            std::find(Esperados.begin(), Esperados.end(), Vid) != Esperados.end();
        if (DeviaEscrever) {
            Check(Escrito(Vid), "o vertice esperado pelos campos de DRAW foi processado");
        } else {
            // Cobre tambem os comandos ALEM do count: eles estao escritos no buffer e as faixas de
            // indice deles sao disjuntas, entao "intocado" aqui e a prova de que o count limitou.
            Check(Vertice(Vid)[0] == kSentinela && Vertice(Vid)[3] == kSentinela,
                  "nenhum vertice fora do conjunto executado foi tocado");
        }
    }

    // O CONTEUDO prova as duas constantes por comando, o BaseVertexLocation e a sobrevivencia das
    // outras tres constantes.
    for (u32 C = 0; C < kExecutedCount; ++C) {
        const FComando& Cmd = kComandos[C];
        for (u32 I = 0; I < Cmd.IndexCountPerInstance; ++I) {
            const u32 Vid = Cmd.StartIndexLocation + I;
            if (Vid >= kObservedSlots / 2u || !Escrito(Vid)) continue;
            const u32* V = Vertice(Vid);
            Check(V[0] == Cmd.MaterialRow,
                  "a linha do material chegou no offset de MaterialRow");
            Check(V[1] == Cmd.InstanceSlot,
                  "o slot da instancia chegou no offset de InstanceSlot");
            // `VB[k] == k`, e o fetch e `VB[IndexBuffer[i] + BaseVertexLocation]`: a diferenca para o
            // SV_VertexID E o campo. E a unica assercao do teste que cobre BaseVertexLocation.
            Check(V[2] == Vid + static_cast<u32>(Cmd.BaseVertexLocation),
                  "o atributo buscado reflete o BaseVertexLocation do comando");
            Check(V[7] == Vid, "SV_VertexID casa com StartIndexLocation + i");
            Check(V[4] == kPasseMaterialTable,
                  "o descritor da tabela de materiais sobreviveu ao ExecuteIndirect");
            Check(V[5] == kPasseTransformSrv,
                  "o descritor da coluna Transform sobreviveu ao ExecuteIndirect");
            Check(V[6] == kPassePrevTransform,
                  "o descritor da coluna PrevTransform sobreviveu ao ExecuteIndirect");
        }
    }

    // Os dois comandos tem de ter trazido valores DIFERENTES. Sem isto, uma command signature que
    // aplicasse o primeiro comando duas vezes — stride zero, por exemplo — passaria em tudo acima.
    {
        const u32 Vid0 = kComandos[0].StartIndexLocation;
        const u32 Vid1 = kComandos[1].StartIndexLocation;
        if (Escrito(Vid0) && Escrito(Vid1)) {
            Check(Vertice(Vid0)[0] != Vertice(Vid1)[0],
                  "o avanco de um stride trouxe outra linha de material");
            Check(Vertice(Vid0)[1] != Vertice(Vid1)[1],
                  "o avanco de um stride trouxe outro slot de instancia");
        }
    }

    if (Failures == 0) std::cout << "  OK\n";
    return Failures == 0 ? 0 : 1;
} catch (const std::exception& E) {
    std::cerr << "  FAIL: " << E.what() << "\n";
    return 1;
}
