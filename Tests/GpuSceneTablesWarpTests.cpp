// ABI das tabelas canonicas da cena, executada em WARP.
//
// O que ele impede: os static_assert de GpuMaterialRecord.h e GpuSceneTables.h provam o layout do
// lado C++ e nao dizem nada sobre o espelho HLSL. Trocar dois campos de lugar em
// Shaders/Scene/GpuSceneTables.hlsli, ou renumerar um SMILE_MAT_FLAG_*, compila dos dois lados e
// so aparece como material trocado em todo hit de RT — o tipo de erro que uma comparacao visual
// estatica pode ate nao mostrar, porque muitos materiais sao parecidos.
//
// Aqui os bytes sao montados pelo C++ de PRODUCAO (FGpuMaterialRecord::FromParts) e lidos pelo
// espelho HLSL de PRODUCAO, compilado pelo MESMO DXC dos shaders do jogo. WARP e o rasterizador de
// software da Microsoft: nao exige placa e cabe no CI.
//
// Cobre tambem a parte CPU pura da tabela: DiffMaterialRows, que e o mecanismo do gate "uma
// edicao de material toca UMA linha".

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Smile/Graphics/Scene/GpuSceneTables.h"

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

    // Constantes com valores DISTINTOS por campo: um campo lido do vizinho aparece como numero
    // errado, e nao como zero que qualquer default tambem produziria.
    MaterialConstants Constantes(f32 Semente) {
        MaterialConstants C{};
        C.BaseColorFactor  = { Semente + 0.1f, Semente + 0.2f, Semente + 0.3f, Semente + 0.4f };
        C.MetallicFactor   = Semente + 0.5f;
        C.RoughnessFactor  = Semente + 0.6f;
        C.EmissiveFactor   = { Semente + 1.0f, Semente + 2.0f, Semente + 3.0f, 1.0f };
        C.EmissiveStrength = 1.0f;
        C.RTEmissiveScale  = 1.0f;
        C.AlphaCutoff      = Semente + 0.7f;
        C.SubsurfaceColor  = { Semente + 0.8f, Semente + 0.9f, Semente + 1.1f, Semente + 1.2f };

        // Bloco de raster (Fase 3). Valores TODOS distintos entre si e distintos dos defaults: com
        // os defaults, trocar dois campos de lugar no espelho passaria verde, que e exatamente o
        // erro que esta sonda existe para pegar.
        C.AOStrength          = Semente + 1.3f;
        C.EmissiveStrength    = Semente + 1.4f;
        C.NormalStrength      = Semente + 1.5f;
        C.HeightScale         = Semente + 1.6f;
        C.ParallaxMinSteps    = Semente + 1.7f;
        C.ParallaxMaxSteps    = Semente + 1.8f;
        C.ParallaxFadeStart   = Semente + 1.9f;
        C.ParallaxFadeRange   = Semente + 2.1f;
        C.ParallaxRefineSteps = 3u + static_cast<u32>(Semente);
        C.ShadingModel        = 2u; // Subsurface: nem DefaultLit nem o Foliage que ja tem bit
        return C;
    }

    bool MesmoFloat(u32 Bits, f32 Valor) {
        u32 Esperado;
        std::memcpy(&Esperado, &Valor, sizeof(u32));
        return Bits == Esperado;
    }

    struct FSonda {
        wrl::ComPtr<ID3D12Device>              Device;
        wrl::ComPtr<ID3D12CommandQueue>        Queue;
        wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
        wrl::ComPtr<ID3D12GraphicsCommandList> List;
        wrl::ComPtr<ID3D12RootSignature>       Root;
        wrl::ComPtr<ID3D12PipelineState>       Pipeline;
        wrl::ComPtr<ID3D12DescriptorHeap>      Heap;
        wrl::ComPtr<ID3D12Fence>               Fence;
        UINT                                   Step = 0;

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
            Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD, Estado, nullptr,
                                               IID_PPV_ARGS(&R)), "CreateBuffer");
            return R;
        }

        wrl::ComPtr<ID3D12Resource> Upload(const void* Dados, u64 Bytes) {
            auto R = Buffer(D3D12_HEAP_TYPE_UPLOAD, Bytes, false,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
            if (Dados) {
                void* P = nullptr;
                Hr(R->Map(0, nullptr, &P), "Map");
                std::memcpy(P, Dados, static_cast<size_t>(Bytes));
                R->Unmap(0, nullptr);
            }
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

    // ---- parte CPU pura: o mecanismo do gate "uma edicao toca uma linha" --------------------
    void TestarDiff() {
        std::vector<FGpuMaterialRecord> Fresh(4);
        for (u32 I = 0; I < 4; ++I)
            Fresh[I] = FGpuMaterialRecord::FromParts(Constantes(static_cast<f32>(I)), false, false,
                                                     100u + I * 8u);

        std::vector<FGpuMaterialRecord> Shadow;
        std::vector<u32> Changed;

        // Primeira publicacao: tudo e novo, porque a GPU nao recebeu linha nenhuma. Este caso
        // existe porque o SetupForMaterials LIMPA o shadow — sem isso, um buffer recem-criado
        // (conteudo indefinido) seria considerado igual ao que a tabela anterior tinha.
        Check(DiffMaterialRows(Fresh, Shadow, Changed) == 4u,
              "primeira publicacao sobe todas as linhas");

        // Nada mudou: nenhuma linha. E o caso comum de TODO frame — o gate exige que o caminho
        // estavel nao suje a tabela sozinho.
        Check(DiffMaterialRows(Fresh, Shadow, Changed) == 0u,
              "sem edicao, nenhuma linha e subida");

        // UMA edicao de material toca UMA linha. E o gate da fase.
        Fresh[2].BaseColor.X += 0.5f;
        Check(DiffMaterialRows(Fresh, Shadow, Changed) == 1u,
              "uma edicao de material toca uma linha");
        Check(!Changed.empty() && Changed[0] == 2u, "e e a linha do material editado");

        // Editar de volta para o valor anterior tambem e uma mudanca: o que a GPU tem e o que o
        // shadow diz, e ele ja foi atualizado.
        Fresh[2].BaseColor.X -= 0.5f;
        Check(DiffMaterialRows(Fresh, Shadow, Changed) == 1u, "desfazer tambem e uma linha");

        // Cada campo tem de participar da comparacao. Um operator== que esquecesse um campo
        // deixaria aquela propriedade parar de chegar a GPU, em silencio.
        struct FCampo { const char* Nome; void (*Aplicar)(FGpuMaterialRecord&); };
        const FCampo kCampos[] = {
            { "BaseColor.W",       [](FGpuMaterialRecord& R) { R.BaseColor.W += 1.0f; } },
            { "EmissiveFactor.Z",  [](FGpuMaterialRecord& R) { R.EmissiveFactor.Z += 1.0f; } },
            { "EmissiveFactor.W",  [](FGpuMaterialRecord& R) { R.EmissiveFactor.W += 1.0f; } },
            { "SubsurfaceColor.Y", [](FGpuMaterialRecord& R) { R.SubsurfaceColor.Y += 1.0f; } },
            { "RoughnessFactor",   [](FGpuMaterialRecord& R) { R.RoughnessFactor += 1.0f; } },
            { "AlphaCutoff",       [](FGpuMaterialRecord& R) { R.AlphaCutoff += 1.0f; } },
            { "Flags",             [](FGpuMaterialRecord& R) { R.Flags ^= kMaterialFlagFoliage; } },
            { "TextureBase",       [](FGpuMaterialRecord& R) { R.TextureBase += 8u; } },
        };
        for (const FCampo& C : kCampos) {
            C.Aplicar(Fresh[1]);
            const u32 N = DiffMaterialRows(Fresh, Shadow, Changed);
            Check(N == 1u && !Changed.empty() && Changed[0] == 1u,
                  std::string("mudar ") + C.Nome + " marca a linha como suja");
        }

        // Crescer a tabela (carga aditiva) publica so as linhas novas.
        Fresh.push_back(FGpuMaterialRecord::FromParts(Constantes(9.0f), true, false, 512u));
        Fresh.push_back(FGpuMaterialRecord{});
        Check(DiffMaterialRows(Fresh, Shadow, Changed) == 2u,
              "linhas novas sobem, as antigas intactas nao");
        Check(Changed.size() == 2 && Changed[0] == 4u && Changed[1] == 5u,
              "e sao exatamente as duas novas");
    }

    // ---- footprint: o staging tambem e persistente -------------------------------------------
    // O primeiro `residentBytes` publicado contava so o buffer DEFAULT e valia um TERCO do que a
    // tabela custa — os slices de upload sao criados junto com ela e ficam mapeados o tempo todo.
    void TestarFootprint() {
        constexpr u32 kCap = 49, kRow = 64;
        const auto F = FGpuTableStorage::FootprintFor(kCap, kRow);
        Check(F.Resident == static_cast<u64>(kCap) * kRow, "residente = capacidade x stride");
        Check(F.Staging == F.Resident * FGpuTableStorage::kFrameSlots,
              "staging = um slice por frame em voo");
        // A relacao que a regressao quebra: o total NAO pode ser igual ao residente.
        Check(F.Total() > F.Resident,
              "o total tem de incluir o staging, e nao so a VRAM");
        Check(F.Total() == F.Resident * (1u + FGpuTableStorage::kFrameSlots),
              "com dois frames em voo, o total e 3x o residente");
        // Tabela vazia nao inventa bytes.
        const auto Z = FGpuTableStorage::FootprintFor(0, kRow);
        Check(Z.Resident == 0 && Z.Staging == 0 && Z.Total() == 0, "capacidade zero custa zero");

        Check(FGpuSceneInstanceTable::kResidentBytesPerSlot == 180u,
              "instancia canonica ocupa 20+32+64+64 = 180 B residentes por slot");
        Check((FGpuTableStorage::kShaderReadState &
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0,
              "tabela autoriza leitura do pixel shader do raster");
        Check((FGpuTableStorage::kShaderReadState &
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0,
              "tabela continua autorizando RT e compute");
    }

    void TestarDiffInstancias() {
        std::vector<FGpuSceneInstanceMeta> Fresh(4);
        for (u32 I = 0; I < 4; ++I) {
            Fresh[I].MeshId = I + 10u;
            Fresh[I].MaterialId = I + 20u;
            Fresh[I].CellId = I + 30u;
            Fresh[I].Flags = GpuSceneInstanceVisible;
            Fresh[I].Generation = I + 1u;
        }
        std::vector<FGpuSceneInstanceMeta> Shadow;
        std::vector<u32> Changed;
        Check(DiffTableRows<FGpuSceneInstanceMeta>(Fresh, Shadow, Changed) == 4u,
              "primeira publicacao da tabela de instancias sobe todas as linhas");
        Check(DiffTableRows<FGpuSceneInstanceMeta>(Fresh, Shadow, Changed) == 0u,
              "instancias estaveis nao sujam a tabela");
        Fresh[2].Generation += 1u;
        Check(DiffTableRows<FGpuSceneInstanceMeta>(Fresh, Shadow, Changed) == 1u &&
                  Changed.size() == 1u && Changed[0] == 2u,
              "reciclar um slot toca somente sua linha canonica");
    }
}

int main() try {
    std::cout << "Smile.GpuSceneTablesWarp\n";

    TestarDiff();
    TestarFootprint();
    TestarDiffInstancias();

    // ---- dados: quatro instancias apontando para tres linhas de material ---------------------
    // A terceira e a quarta compartilham a linha 2 de proposito: e a situacao que a fase existe
    // para permitir (N instancias, um material), e ela tambem prova que MaterialId de verdade
    // enderaca a tabela em vez de coincidir com o indice da instancia.
    std::vector<FGpuMaterialRecord> Linhas;
    Linhas.push_back(FGpuMaterialRecord{}); // linha 0 = material default
    {
        MaterialConstants C = Constantes(1.0f);
        C.HasAlbedoMap = 1u;
        C.HasEmissiveMap = 1u;
        // Tres dos seis bits novos LIGADOS e tres desligados, para que renumerar um deles nao
        // possa passar por coincidencia de "tudo zero" ou "tudo um".
        C.HasNormalMap        = 1u;
        C.HasHeightMap        = 1u;
        C.NormalReconstructZ  = 1u;
        Linhas.push_back(FGpuMaterialRecord::FromParts(C, /*TwoSided*/ true, /*Blend*/ false, 4096u));
    }
    {
        MaterialConstants C = Constantes(2.0f);
        C.AlphaTest = 1u;
        // Os outros tres bits novos, complementares aos da linha acima.
        C.HasAOMap        = 1u;
        C.NormalFlipY     = 1u;
        C.ParallaxRefine  = 1u;
        Linhas.push_back(FGpuMaterialRecord::FromParts(C, /*TwoSided*/ false, /*Blend*/ true, 8192u));
    }

    // Linhas de malha. A linha 0 e a malha AUSENTE (tudo zero), como a tabela real a reserva.
    std::vector<FGpuMeshRecord> Malhas(3);
    for (u32 I = 1; I < 3; ++I) {
        const f32 S = static_cast<f32>(I);
        Malhas[I].LocalAABBMin  = { -S, -S - 0.5f, -S - 0.25f };
        Malhas[I].LocalAABBMax  = {  S,  S + 0.5f,  S + 0.25f };
        Malhas[I].VertexSrv     = 100u + I * 3u;
        Malhas[I].IndexSrv      = 101u + I * 3u;
        Malhas[I].TriangleSrv   = 102u + I * 3u;
        Malhas[I].VertexCount   = 1000u + I;
        Malhas[I].IndexCount    = 2000u + I;
        Malhas[I].TriangleCount = 3000u + I;
    }

    std::vector<FGpuInstanceGeo> Instancias(4);
    for (u32 I = 0; I < 4; ++I) {
        Instancias[I].TriangleSrv = 12u + I * 3u;
        Instancias[I].MaterialSrv = 777u;
        Instancias[I].MeshSrv     = 888u;
    }
    Instancias[0].MaterialId = 0u; Instancias[0].MeshId = 0u; // sem material e sem malha
    Instancias[1].MaterialId = 1u; Instancias[1].MeshId = 1u;
    Instancias[2].MaterialId = 2u; Instancias[2].MeshId = 2u;
    // As duas ultimas compartilham material E malha: e a situacao que a fase existe para permitir
    // (N instancias, uma linha de cada), e ela prova que os ids enderecam as tabelas em vez de
    // coincidirem com o indice da instancia.
    Instancias[3].MaterialId = 2u; Instancias[3].MeshId = 2u;
    const u32 kInstancias = static_cast<u32>(Instancias.size());

    std::vector<FGpuSceneInstanceMeta> Meta(kInstancias);
    std::vector<FGpuSceneInstanceBounds> Bounds(kInstancias);
    std::vector<FGpuSceneInstanceTransform> Current(kInstancias), Previous(kInstancias);
    for (u32 I = 0; I < kInstancias; ++I) {
        Meta[I].MeshId     = 30u + I;
        Meta[I].MaterialId = 40u + I;
        Meta[I].CellId     = 50u + I;
        Meta[I].Generation = 60u + I;
        Meta[I].Flags = GpuSceneInstanceVisible |
                        (I & 1u ? GpuSceneInstanceRaytracingOnly : 0u) |
                        (I & 2u ? GpuSceneInstanceDynamic : 0u);
        const f32 S = static_cast<f32>(I) * 10.0f;
        Bounds[I].Center = { S + 0.1f, S + 0.2f, S + 0.3f, S + 0.4f };
        Bounds[I].Extent = { S + 1.1f, S + 1.2f, S + 1.3f, S + 1.4f };
        for (u32 Row = 0; Row < 4; ++Row) {
            for (u32 Col = 0; Col < 4; ++Col) {
                Current[I].Value.M[Row][Col] = S + 10.0f + static_cast<f32>(Row * 4u + Col);
                Previous[I].Value.M[Row][Col] = S + 100.0f + static_cast<f32>(Row * 4u + Col);
            }
        }
    }

    // ---- WARP --------------------------------------------------------------------------------
    FSonda S;
    wrl::ComPtr<IDXGIFactory4> Factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1 (WARP obrigatorio)");
    wrl::ComPtr<IDXGIAdapter> Warp;
    Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter (WARP obrigatorio)");
    Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&S.Device)),
       "D3D12CreateDevice (WARP obrigatorio)");

    const std::vector<u8> Bytecode = LerArquivo(SMILE_SCENE_TABLES_PROBE_CSO);
    Check(!Bytecode.empty(), "a sonda compilada foi encontrada");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_SCENE_TABLES_PROBE_CSO << ")\n";
        return 1;
    }

    D3D12_DESCRIPTOR_RANGE Ranges[2]{};
    Ranges[0].RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Ranges[0].NumDescriptors = 7;
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
    HD.NumDescriptors = 8;
    HD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Hr(S.Device->CreateDescriptorHeap(&HD, IID_PPV_ARGS(&S.Heap)), "CreateDescriptorHeap");
    S.Step = S.Device->GetDescriptorHandleIncrementSize(HD.Type);

    auto BufLinhas = S.Upload(Linhas.data(), Linhas.size() * sizeof(FGpuMaterialRecord));
    auto BufInst   = S.Upload(Instancias.data(), Instancias.size() * sizeof(FGpuInstanceGeo));
    auto BufMalhas = S.Upload(Malhas.data(), Malhas.size() * sizeof(FGpuMeshRecord));
    auto BufMeta = S.Upload(Meta.data(), Meta.size() * sizeof(FGpuSceneInstanceMeta));
    auto BufBounds = S.Upload(Bounds.data(), Bounds.size() * sizeof(FGpuSceneInstanceBounds));
    auto BufCurrent = S.Upload(Current.data(), Current.size() * sizeof(FGpuSceneInstanceTransform));
    auto BufPrevious = S.Upload(Previous.data(), Previous.size() * sizeof(FGpuSceneInstanceTransform));
    constexpr u32 kResultRows = 26u;
    constexpr u32 kResultStride = kResultRows * 4u;
    const u64 SaidaBytes = static_cast<u64>(kInstancias) * kResultStride * sizeof(u32);
    auto BufSaida   = S.Buffer(D3D12_HEAP_TYPE_DEFAULT, SaidaBytes, true,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto BufLeitura = S.Buffer(D3D12_HEAP_TYPE_READBACK, SaidaBytes, false,
                               D3D12_RESOURCE_STATE_COPY_DEST);

    S.SRV(BufLinhas.Get(), static_cast<u32>(Linhas.size()), sizeof(FGpuMaterialRecord), 0);
    S.SRV(BufInst.Get(), kInstancias, sizeof(FGpuInstanceGeo), 1);
    S.SRV(BufMalhas.Get(), static_cast<u32>(Malhas.size()), sizeof(FGpuMeshRecord), 2);
    S.SRV(BufMeta.Get(), kInstancias, sizeof(FGpuSceneInstanceMeta), 3);
    S.SRV(BufBounds.Get(), kInstancias, sizeof(FGpuSceneInstanceBounds), 4);
    S.SRV(BufCurrent.Get(), kInstancias, sizeof(FGpuSceneInstanceTransform), 5);
    S.SRV(BufPrevious.Get(), kInstancias, sizeof(FGpuSceneInstanceTransform), 6);
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC UD{};
        UD.Format                     = DXGI_FORMAT_UNKNOWN;
        UD.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        UD.Buffer.NumElements         = kInstancias * kResultRows;
        UD.Buffer.StructureByteStride = sizeof(u32) * 4;
        auto H = S.Heap->GetCPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<SIZE_T>(7) * S.Step;
        S.Device->CreateUnorderedAccessView(BufSaida.Get(), nullptr, &UD, H);
    }

    alignas(16) u8 CB[256]{};
    const u32 Constantes4[4]{ kInstancias, 0u, 0u, 0u };
    std::memcpy(CB, Constantes4, sizeof(Constantes4));
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
    auto GpuAt = [&](UINT I) {
        auto H = S.Heap->GetGPUDescriptorHandleForHeapStart();
        H.ptr += static_cast<UINT64>(I) * S.Step;
        return H;
    };
    S.List->SetComputeRootDescriptorTable(1, GpuAt(0));
    S.List->SetComputeRootDescriptorTable(2, GpuAt(7));
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

    const u32* Saida = nullptr;
    Hr(BufLeitura->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u32**>(&Saida))),
       "Map saida");

    for (u32 I = 0; I < kInstancias; ++I) {
        const FGpuInstanceGeo&    G = Instancias[I];
        const FGpuMaterialRecord& M = Linhas[G.MaterialId];
        const FGpuMeshRecord&     H = Malhas[G.MeshId];
        const FGpuSceneInstanceMeta& IM = Meta[I];
        const FGpuSceneInstanceBounds& IB = Bounds[I];
        const u32* R = Saida + static_cast<size_t>(I) * kResultStride;
        const std::string Onde = "instancia " + std::to_string(I);

        Check(MesmoFloat(R[0], M.BaseColor.X) && MesmoFloat(R[1], M.BaseColor.Y) &&
              MesmoFloat(R[2], M.BaseColor.Z) && MesmoFloat(R[3], M.BaseColor.W),
              Onde + ": BaseColor no offset 0");
        Check(MesmoFloat(R[4], M.EmissiveFactor.X) && MesmoFloat(R[5], M.EmissiveFactor.Y) &&
              MesmoFloat(R[6], M.EmissiveFactor.Z) && MesmoFloat(R[7], M.EmissiveFactor.W),
              Onde + ": EmissiveFactor no offset 16");
        Check(MesmoFloat(R[8], M.SubsurfaceColor.X) && MesmoFloat(R[9], M.SubsurfaceColor.Y) &&
              MesmoFloat(R[10], M.SubsurfaceColor.Z) && MesmoFloat(R[11], M.SubsurfaceColor.W),
              Onde + ": SubsurfaceColor no offset 32");
        Check(MesmoFloat(R[12], M.RoughnessFactor), Onde + ": RoughnessFactor no offset 48");
        Check(MesmoFloat(R[13], M.AlphaCutoff),     Onde + ": AlphaCutoff no offset 52");
        Check(R[14] == M.Flags,                     Onde + ": Flags no offset 56");
        Check(R[15] == M.TextureBase,               Onde + ": TextureBase no offset 60");

        Check(R[16] == G.TriangleSrv, Onde + ": TriangleSrv no offset 0");
        Check(R[17] == G.MeshSrv,     Onde + ": MeshSrv no offset 4");
        Check(R[18] == G.MeshId,      Onde + ": MeshId no offset 8");
        Check(R[19] == G.MaterialSrv, Onde + ": MaterialSrv no offset 12");
        Check(R[20] == G.MaterialId,  Onde + ": MaterialId no offset 16");

        // Constantes compartilhadas: deslocamento de textura e dois bits de flag. Sem estes tres,
        // renumerar um SMILE_MAT_FLAG_* passaria verde — o layout continuaria certo.
        Check(R[21] == M.TextureIndex(kMaterialTexEmissive),
              Onde + ": MaterialTextureIndex(EMISSIVE) casa com o C++");
        Check((R[22] != 0u) == ((M.Flags & kMaterialFlagTwoSided) != 0u),
              Onde + ": SMILE_MAT_FLAG_TWOSIDED casa com o C++");
        Check((R[23] != 0u) == ((M.Flags & kMaterialFlagAlbedoMap) != 0u),
              Onde + ": SMILE_MAT_FLAG_ALBEDOMAP casa com o C++");

        // Linha da malha. float3 em StructuredBuffer ocupa 12 B e NAO e promovido a 16 — e o que
        // faz LocalAABBMax cair no offset 12 e VertexSrv no 24. Se o DXC mudasse isso, os quatro
        // primeiros campos ainda casariam e todo o resto andaria.
        Check(MesmoFloat(R[24], H.LocalAABBMin.X) && MesmoFloat(R[25], H.LocalAABBMin.Y) &&
              MesmoFloat(R[26], H.LocalAABBMin.Z), Onde + ": LocalAABBMin no offset 0");
        Check(MesmoFloat(R[27], H.LocalAABBMax.X) && MesmoFloat(R[28], H.LocalAABBMax.Y) &&
              MesmoFloat(R[29], H.LocalAABBMax.Z), Onde + ": LocalAABBMax no offset 12");
        Check(R[30] == H.VertexSrv,     Onde + ": VertexSrv da malha no offset 24");
        Check(R[31] == H.IndexSrv,      Onde + ": IndexSrv da malha no offset 28");
        Check(R[32] == H.TriangleSrv,   Onde + ": TriangleSrv da malha no offset 32");
        Check(R[33] == H.VertexCount,   Onde + ": VertexCount no offset 36");
        Check(R[34] == H.IndexCount,    Onde + ": IndexCount no offset 40");
        Check(R[35] == H.TriangleCount, Onde + ": TriangleCount no offset 44");

        Check(R[36] == IM.MeshId && R[37] == IM.MaterialId && R[38] == IM.CellId &&
                  R[39] == IM.Flags,
              Onde + ": linha meta nos offsets 0..12");
        Check(R[40] == IM.Generation, Onde + ": generation da instancia no offset 16");
        Check((R[41] != 0u) == ((IM.Flags & GpuSceneInstanceVisible) != 0u),
              Onde + ": SMILE_INSTANCE_FLAG_VISIBLE casa com o C++");
        Check((R[42] != 0u) == ((IM.Flags & GpuSceneInstanceRaytracingOnly) != 0u),
              Onde + ": SMILE_INSTANCE_FLAG_RT_ONLY casa com o C++");
        Check((R[43] != 0u) == ((IM.Flags & GpuSceneInstanceDynamic) != 0u),
              Onde + ": SMILE_INSTANCE_FLAG_DYNAMIC casa com o C++");
        Check(MesmoFloat(R[44], IB.Center.X) && MesmoFloat(R[45], IB.Center.Y) &&
                  MesmoFloat(R[46], IB.Center.Z) && MesmoFloat(R[47], IB.Center.W),
              Onde + ": bounds center no offset 0");
        Check(MesmoFloat(R[48], IB.Extent.X) && MesmoFloat(R[49], IB.Extent.Y) &&
                  MesmoFloat(R[50], IB.Extent.Z) && MesmoFloat(R[51], IB.Extent.W),
              Onde + ": bounds extent no offset 16");
        for (u32 K = 0; K < 16; ++K) {
            Check(MesmoFloat(R[52 + K], Current[I].Value.M[K / 4u][K % 4u]),
                  Onde + ": matriz atual no indice " + std::to_string(K));
            Check(MesmoFloat(R[68 + K], Previous[I].Value.M[K / 4u][K % 4u]),
                  Onde + ": matriz anterior no indice " + std::to_string(K));
        }

        // Bloco de raster da linha de material (Fase 3), offsets 64..108. Um deslocamento aqui
        // nao apareceria como material trocado: apareceria como parallax, forca de normal ou
        // modelo de sombreamento de OUTRO material — bem mais dificil de atribuir numa captura.
        Check(MesmoFloat(R[84], M.RasterEmissive.X) && MesmoFloat(R[85], M.RasterEmissive.Y) &&
              MesmoFloat(R[86], M.RasterEmissive.Z) && MesmoFloat(R[87], M.RasterEmissive.W),
              Onde + ": RasterEmissive no offset 64");
        Check(MesmoFloat(R[88], M.NormalStrength),   Onde + ": NormalStrength no offset 80");
        Check(MesmoFloat(R[89], M.HeightScale),      Onde + ": HeightScale no offset 84");
        Check(MesmoFloat(R[90], M.ParallaxMinSteps), Onde + ": ParallaxMinSteps no offset 88");
        Check(MesmoFloat(R[91], M.ParallaxMaxSteps), Onde + ": ParallaxMaxSteps no offset 92");
        Check(MesmoFloat(R[92], M.ParallaxFadeStart), Onde + ": ParallaxFadeStart no offset 96");
        Check(MesmoFloat(R[93], M.ParallaxFadeRange), Onde + ": ParallaxFadeRange no offset 100");
        Check(R[94] == M.ParallaxRefineSteps, Onde + ": ParallaxRefineSteps no offset 104");
        Check(R[95] == M.ShadingModel,        Onde + ": ShadingModel no offset 108");

        Check((R[96] != 0u) == ((M.Flags & kMaterialFlagNormalMap) != 0u),
              Onde + ": SMILE_MAT_FLAG_NORMALMAP casa com o C++");
        Check((R[97] != 0u) == ((M.Flags & kMaterialFlagAOMap) != 0u),
              Onde + ": SMILE_MAT_FLAG_AOMAP casa com o C++");
        Check((R[98] != 0u) == ((M.Flags & kMaterialFlagHeightMap) != 0u),
              Onde + ": SMILE_MAT_FLAG_HEIGHTMAP casa com o C++");
        Check((R[99] != 0u) == ((M.Flags & kMaterialFlagNormalFlipY) != 0u),
              Onde + ": SMILE_MAT_FLAG_NORMALFLIPY casa com o C++");
        Check((R[100] != 0u) == ((M.Flags & kMaterialFlagNormalReconZ) != 0u),
              Onde + ": SMILE_MAT_FLAG_NORMALRECONZ casa com o C++");
        Check((R[101] != 0u) == ((M.Flags & kMaterialFlagParallaxRefine) != 0u),
              Onde + ": SMILE_MAT_FLAG_PARALLAXREFINE casa com o C++");
        Check(R[102] == M.TextureIndex(kMaterialTexNormal),
              Onde + ": MaterialTextureIndex(NORMAL) casa com o C++");
        Check(R[103] == M.TextureIndex(kMaterialTexHeight),
              Onde + ": MaterialTextureIndex(HEIGHT) casa com o C++");
    }
    BufLeitura->Unmap(0, nullptr);

    if (Failures == 0) {
        std::cout << "  " << kInstancias
                  << " instancias x (material + malha + colunas de instancia) lidas em WARP, todas OK\n";
        return 0;
    }
    std::cerr << "  " << Failures << " falha(s)\n";
    return 1;
} catch (const std::exception& E) {
    std::cerr << "Smile.GpuSceneTablesWarp: excecao -- " << E.what() << '\n';
    return 2;
}
