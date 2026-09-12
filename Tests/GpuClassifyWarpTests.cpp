// Classificacao e contagem por lote, executadas em WARP (Fase 5 do GPU-DRIVEN-PLAN).
//
// O que este teste existe para impedir, em ordem de gravidade:
//
//  1. um candidato ser contado DUAS vezes, ou nenhuma. A divisao "indireto x fallback" so e segura
//     porque cada candidato tem exatamente um desfecho: quem a GPU recusa, a CPU desenha. Contar
//     duas vezes desenha o objeto duas vezes; nao contar o faz sumir, que a regra 6 proibe;
//  2. a leitura de uma linha ANTES de validar o indice. Load fora do StructuredBuffer nao trava —
//     o D3D12 devolve zero —, e zero e a linha do material default e a da malha ausente. O
//     candidato passaria a desenhar algo PLAUSIVEL e errado;
//  3. a porta do indireto ser partida em dois testes. So o predicado completo pode decidir; a
//     decomposicao existe para ATRIBUIR o motivo, e um dia em que ela decidisse mandaria a malha
//     irrepresentavel para o command processor com offset poison;
//  4. os indices dos desfechos divergirem entre C++ e HLSL. Sao dois espelhos de macros e nada os
//     liga em tempo de compilacao; aqui os nove sao lidos de volta.
//
// O shader e o de PRODUCAO, compilado pelo MESMO DXC dos shaders do jogo. WARP nao exige placa.
//
// A camada de debug PARTICIPA do resultado: ERROR e CORRUPTION reprovam, e sao drenados DEPOIS da
// fence — antes dela a validacao da execucao ainda nao existe.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Smile/Graphics/Scene/GpuClassify.h"
#include "Smile/Graphics/Scene/GpuSceneCandidates.h"

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
        std::ifstream F(_Caminho, std::ios::binary);
        if (!F) return {};
        return std::vector<u8>((std::istreambuf_iterator<char>(F)),
                               std::istreambuf_iterator<char>());
    }

    const char* NomeDesfecho(u32 _O) {
        switch (_O) {
            case GpuClassifyIndirect:          return "Indirect";
            case GpuClassifyNoSlot:            return "NoSlot";
            case GpuClassifyInvalidSlot:       return "InvalidSlot";
            case GpuClassifyInvalidMaterialId: return "InvalidMaterialId";
            case GpuClassifyInvalidMeshId:     return "InvalidMeshId";
            case GpuClassifyInvalidPoolId:     return "InvalidPoolId";
            case GpuClassifyNotEligible:       return "NotEligible";
            case GpuClassifyUnrepresentable:   return "Unrepresentable";
            case GpuClassifyOverflow:          return "Overflow";
            default:                           return "?";
        }
    }

    // ---- a cena do teste ----------------------------------------------------------------------
    //
    // Montada para que TODO desfecho seja alcancado por pelo menos um candidato, e para que dois
    // candidatos caiam no MESMO lote — sem isso, um shader que sobrescrevesse em vez de somar
    // passaria.
    constexpr u32 kPoolCount = 2u;

    std::vector<FGpuMaterialRecord> Materiais() {
        std::vector<FGpuMaterialRecord> M;
        MaterialConstants C{};
        M.push_back(FGpuMaterialRecord{});                                   // 0: one-sided opaco
        M.push_back(FGpuMaterialRecord::FromParts(C, true, false, 0u));      // 1: two-sided
        C.AlphaTest = 1u;
        M.push_back(FGpuMaterialRecord::FromParts(C, true, false, 0u));      // 2: masked
        C.AlphaTest = 0u;
        M.push_back(FGpuMaterialRecord::FromParts(C, true, true, 0u));       // 3: blend
        return M;
    }

    std::vector<FGpuMeshRecord> Malhas() {
        std::vector<FGpuMeshRecord> H(4);
        H[0].GeometryPoolId = 0u;
        H[1].GeometryPoolId = 1u;
        H[2].GeometryPoolId = 0u;
        H[2].Flags          = GpuMeshOffsetsUnrepresentable;
        H[3].GeometryPoolId = 7u; // fora da lista publicada: kPoolCount e 2
        return H;
    }

    // Doze linhas, uma por situacao. A generation e 1 em todas menos na ultima, que fica VAZIA.
    std::vector<FGpuSceneInstanceMeta> Meta() {
        std::vector<FGpuSceneInstanceMeta> M(12);
        for (auto& R : M) { R.Generation = 1u; R.Flags = GpuSceneInstanceVisible; }
        const u32 Mat[12]  = { 0, 0, 0, 1, 2, 3, 0,  1, 0, 99, 0,  0 };
        const u32 Mesh[12] = { 0, 1, 0, 0, 0, 0, 2,  2, 3,  0, 99, 0 };
        for (u32 I = 0; I < 12u; ++I) { M[I].MaterialId = Mat[I]; M[I].MeshId = Mesh[I]; }
        M[11].Generation = 0u; // linha sem ocupante
        return M;
    }

    // Os catorze candidatos: as doze linhas, mais um buraco do expand e um slot fora da tabela.
    std::vector<u32> Slots() {
        std::vector<u32> S;
        for (u32 I = 0; I < 12u; ++I) S.push_back(I);
        S.push_back(kGpuExpandNoSlot); // buraco deixado pelo expand
        S.push_back(999u);             // slot fora da tabela de meta
        return S;
    }

    // ---- parte CPU pura: o desfecho de cada candidato -----------------------------------------
    //
    // O oraculo e uma TABELA escrita a mao, e nao a funcao rodada de novo: a referencia CPU e o
    // shader sao dois espelhos do mesmo raciocinio, e compara-los entre si nao pegaria um raciocinio
    // errado nos dois.
    void TestarDesfechos() {
        const auto Mats  = Materiais();
        const auto Meshs = Malhas();
        const auto Metas = Meta();
        const auto S     = Slots();

        const u32 kEsperado[14] = {
            GpuClassifyIndirect,          // 0: one-sided, pool 0
            GpuClassifyIndirect,          // 1: one-sided, pool 1
            GpuClassifyIndirect,          // 2: one-sided, pool 0 de novo (mesmo lote que o 0)
            GpuClassifyNotEligible,       // 3: two-sided
            GpuClassifyNotEligible,       // 4: masked
            GpuClassifyNotEligible,       // 5: blend
            GpuClassifyUnrepresentable,   // 6: classe elegivel, malha com a flag
            GpuClassifyNotEligible,       // 7: os DOIS termos fechados -> a classe tem precedencia
            GpuClassifyInvalidPoolId,     // 8: pool fora da lista
            GpuClassifyInvalidMaterialId, // 9
            GpuClassifyInvalidMeshId,     // 10
            GpuClassifyInvalidSlot,       // 11: linha sem ocupante
            GpuClassifyNoSlot,            // 12: buraco do expand
            GpuClassifyInvalidSlot,       // 13: slot fora da tabela
        };
        for (u32 I = 0; I < 14u; ++I) {
            FGpuBatchKey Key{};
            const u32 Got = GpuClassifyCandidate(S[I], Metas, Mats, Meshs, kPoolCount, Key);
            Check(Got == kEsperado[I],
                  std::string("candidato ") + std::to_string(I) + ": desfecho " +
                      NomeDesfecho(Got) + ", esperado " + NomeDesfecho(kEsperado[I]));
        }
    }

    // ---- parte CPU pura: o indice do lote -----------------------------------------------------
    void TestarIndiceDeLote() {
        CheckEq(GpuBatchSlotCount(kPoolCount), kGpuPipelineClassCount * kPoolCount,
                "a tabela de lotes reserva as quatro classes");
        // CLASS-MAJOR: os lotes de uma classe sao contiguos, e classes diferentes nunca colidem.
        CheckEq(GpuBatchIndex({ GpuPipelineClassOpaqueOneSided, 0u }, kPoolCount), 0u, "lote {0,0}");
        CheckEq(GpuBatchIndex({ GpuPipelineClassOpaqueOneSided, 1u }, kPoolCount), 1u, "lote {0,1}");
        CheckEq(GpuBatchIndex({ GpuPipelineClassOpaqueTwoSided, 0u }, kPoolCount), 2u, "lote {1,0}");
        CheckEq(GpuBatchIndex({ GpuPipelineClassBlend, 1u }, kPoolCount), 7u, "lote {3,1}");
        // Todos os pares distintos dao indices distintos e dentro da capacidade. Um indice
        // pool-major com class-major misturados colidiria aqui.
        std::vector<u32> Vistos;
        for (u32 C = 0; C < kGpuPipelineClassCount; ++C)
            for (u32 P = 0; P < kPoolCount; ++P) {
                const u32 Idx = GpuBatchIndex({ C, P }, kPoolCount);
                Check(Idx < GpuBatchSlotCount(kPoolCount), "indice de lote dentro da capacidade");
                Vistos.push_back(Idx);
            }
        std::sort(Vistos.begin(), Vistos.end());
        Check(std::unique(Vistos.begin(), Vistos.end()) == Vistos.end(),
              "pares distintos dao indices distintos");
    }

    // ---- parte CPU pura: exatamente um desfecho por candidato ---------------------------------
    //
    // O invariante que autoriza a divisao indireto/fallback. A soma dos nove contadores tem de dar o
    // numero de candidatos: um desfecho que nao incrementasse nada, ou um caminho que incrementasse
    // dois, aparece aqui e em nenhum outro lugar.
    void TestarExatamenteUmaVez() {
        const auto Mats = Materiais(); const auto Meshs = Malhas();
        const auto Metas = Meta();     const auto S = Slots();

        FGpuClassifyInput In{};
        In.Slots = S; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
        In.PoolCount = kPoolCount; In.BatchCapacity = GpuBatchSlotCount(kPoolCount);

        std::vector<u32> Lotes, Contadores;
        const u32 Indiretos = ClassifyCandidates(In, Lotes, Contadores);

        u32 Soma = 0;
        for (const u32 C : Contadores) Soma += C;
        CheckEq(Soma, static_cast<u32>(S.size()), "todo candidato tem exatamente um desfecho");
        CheckEq(Indiretos, Contadores[GpuClassifyIndirect], "o retorno e o contador de indiretos");

        u32 SomaLotes = 0;
        for (const u32 L : Lotes) SomaLotes += L;
        CheckEq(SomaLotes, Contadores[GpuClassifyIndirect],
                "a soma dos lotes e o numero de comandos");
    }
}

namespace {
    struct FResultado {
        std::vector<u32> Lotes;
        std::vector<u32> Contadores;
    };

    // A sonda. Mesma forma da do expand; o que muda sao as views e o CB.
    struct FSonda {
        wrl::ComPtr<ID3D12Device>        Device;
        wrl::ComPtr<ID3D12CommandQueue>  Queue;
        wrl::ComPtr<ID3D12RootSignature> Root;
        wrl::ComPtr<ID3D12PipelineState> Pipeline;
        wrl::ComPtr<ID3D12Fence>         Fence;
        wrl::ComPtr<ID3D12InfoQueue>     Info;
        u64  FenceValue = 0;
        UINT Step       = 0;

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
            Hr(Device->CreateCommittedResource(&HP, D3D12_HEAP_FLAG_NONE, &RD, _Estado, nullptr,
                                               IID_PPV_ARGS(&R)), "CreateCommittedResource");
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

        // ERROR e CORRUPTION REPROVAM, e nao apenas aparecem no log. Drenada sempre DEPOIS da
        // fence: antes dela a validacao da execucao ainda nao aconteceu.
        void Drenar(const char* _Quando) {
            if (!Info) return;
            const UINT64 N = Info->GetNumStoredMessages();
            for (UINT64 I = 0; I < N; ++I) {
                SIZE_T Bytes = 0;
                if (FAILED(Info->GetMessage(I, nullptr, &Bytes))) continue;
                std::vector<u8> Buf(Bytes);
                auto* Msg = reinterpret_cast<D3D12_MESSAGE*>(Buf.data());
                if (FAILED(Info->GetMessage(I, Msg, &Bytes))) continue;
                if (Msg->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
                std::cerr << "  [D3D12/" << _Quando << "] " << Msg->pDescription << "\n";
                if (Msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    ++Failures;
                    std::cerr << "  FAIL: a camada de debug reprovou (severidade "
                              << static_cast<int>(Msg->Severity) << ")\n";
                }
            }
            Info->ClearStoredMessages();
        }

        void Construir(const std::vector<u8>& _Bytecode) {
            wrl::ComPtr<ID3D12Debug> Debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug)))) Debug->EnableDebugLayer();

            wrl::ComPtr<IDXGIFactory4> Factory;
            Hr(CreateDXGIFactory1(IID_PPV_ARGS(&Factory)), "CreateDXGIFactory1");
            wrl::ComPtr<IDXGIAdapter> Warp;
            Hr(Factory->EnumWarpAdapter(IID_PPV_ARGS(&Warp)), "EnumWarpAdapter");
            Hr(D3D12CreateDevice(Warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)),
               "D3D12CreateDevice");
            Device.As(&Info);
            Check(Info != nullptr, "a camada de debug esta ativa");

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
            Drenar("criacao");
        }

        FResultado Executar(const FGpuClassifyInput& _In) {
            // Piso de UM ELEMENTO, e nao de um byte: um SRV de um elemento sobre um recurso de 1 B
            // e ilegal e desfaz o device no dispatch seguinte, longe da causa.
            auto Pisa = [](u64 _Bytes, u64 _Stride) { return std::max<u64>(_Stride, _Bytes); };
            auto BufSlots = Upload(_In.Slots.data(),
                                   Pisa(_In.Slots.size() * sizeof(u32), sizeof(u32)));
            auto BufMeta  = Upload(_In.Meta.data(),
                                   Pisa(_In.Meta.size() * sizeof(FGpuSceneInstanceMeta),
                                        sizeof(FGpuSceneInstanceMeta)));
            auto BufMats  = Upload(_In.Materials.data(),
                                   Pisa(_In.Materials.size() * sizeof(FGpuMaterialRecord),
                                        sizeof(FGpuMaterialRecord)));
            auto BufMesh  = Upload(_In.Meshes.data(),
                                   Pisa(_In.Meshes.size() * sizeof(FGpuMeshRecord),
                                        sizeof(FGpuMeshRecord)));

            const u64 LoteBytes = Pisa(_In.BatchCapacity * sizeof(u32), sizeof(u32));
            const u64 ContBytes = GpuClassifyOutcomeCount * sizeof(u32);
            // COMMON, e nao COPY_DEST: o D3D12 IGNORA o estado inicial de um buffer (a camada de
            // debug avisa), e todo buffer nasce em COMMON. A copia de zeros logo abaixo promove
            // implicitamente para COPY_DEST, que e de onde a barreira seguinte parte.
            auto BufLotes = Buffer(D3D12_HEAP_TYPE_DEFAULT, LoteBytes, true,
                                   D3D12_RESOURCE_STATE_COMMON);
            auto BufCont  = Buffer(D3D12_HEAP_TYPE_DEFAULT, ContBytes, true,
                                   D3D12_RESOURCE_STATE_COMMON);
            // Os zeros vem de uma COPIA: um DEFAULT recem-criado nao promete conteudo, e um
            // histograma somado sobre lixo daria numeros plausiveis e falsos.
            const std::vector<u32> ZerosLote(static_cast<size_t>(LoteBytes / 4), 0u);
            const u32 ZerosCont[GpuClassifyOutcomeCount]{};
            auto BufZerosLote = Upload(ZerosLote.data(), LoteBytes);
            auto BufZerosCont = Upload(ZerosCont, ContBytes);

            auto LeituraLotes = Buffer(D3D12_HEAP_TYPE_READBACK, LoteBytes, false,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
            auto LeituraCont  = Buffer(D3D12_HEAP_TYPE_READBACK, ContBytes, false,
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
            auto SRV = [&](ID3D12Resource* R, u64 Elementos, u32 Stride, UINT Slot) {
                D3D12_SHADER_RESOURCE_VIEW_DESC D{};
                D.Format                     = DXGI_FORMAT_UNKNOWN;
                D.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                D.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                D.Buffer.NumElements         = std::max<u32>(1u, static_cast<u32>(Elementos));
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
            SRV(BufSlots.Get(), _In.Slots.size(), sizeof(u32), 0);
            SRV(BufMeta.Get(), _In.Meta.size(), sizeof(FGpuSceneInstanceMeta), 1);
            SRV(BufMats.Get(), _In.Materials.size(), sizeof(FGpuMaterialRecord), 2);
            SRV(BufMesh.Get(), _In.Meshes.size(), sizeof(FGpuMeshRecord), 3);
            UAV(BufLotes.Get(), _In.BatchCapacity, 4);
            UAV(BufCont.Get(), GpuClassifyOutcomeCount, 5);

            alignas(16) u8 CB[256]{};
            const u32 P8[8]{ static_cast<u32>(_In.Slots.size()),
                             static_cast<u32>(_In.Meta.size()),
                             static_cast<u32>(_In.Materials.size()),
                             static_cast<u32>(_In.Meshes.size()),
                             _In.PoolCount, _In.BatchCapacity, 0u, 0u };
            std::memcpy(CB, P8, sizeof(P8));
            auto BufCB = Upload(CB, sizeof(CB));

            wrl::ComPtr<ID3D12CommandAllocator>    Allocator;
            wrl::ComPtr<ID3D12GraphicsCommandList> List;
            Hr(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&Allocator)), "CreateCommandAllocator");
            Hr(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(),
                                         Pipeline.Get(), IID_PPV_ARGS(&List)), "CreateCommandList");

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
            List->CopyBufferRegion(BufLotes.Get(), 0, BufZerosLote.Get(), 0, LoteBytes);
            List->CopyBufferRegion(BufCont.Get(), 0, BufZerosCont.Get(), 0, ContBytes);
            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
            const u32 Total = static_cast<u32>(_In.Slots.size());
            if (Total > 0) List->Dispatch((Total + 63u) / 64u, 1, 1);

            Transicao(BufLotes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transicao(BufCont.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
            List->CopyResource(LeituraLotes.Get(), BufLotes.Get());
            List->CopyResource(LeituraCont.Get(), BufCont.Get());
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
                    throw std::runtime_error("dispatch de classificacao nao terminou");
            }
            Drenar("dispatch");

            FResultado Saida;
            Saida.Lotes.resize(_In.BatchCapacity);
            Saida.Contadores.resize(GpuClassifyOutcomeCount);
            if (_In.BatchCapacity > 0) {
                const u32* P = nullptr;
                Hr(LeituraLotes->Map(0, nullptr,
                                     reinterpret_cast<void**>(const_cast<u32**>(&P))), "Map lotes");
                std::memcpy(Saida.Lotes.data(), P, _In.BatchCapacity * sizeof(u32));
                LeituraLotes->Unmap(0, nullptr);
            }
            {
                const u32* P = nullptr;
                Hr(LeituraCont->Map(0, nullptr,
                                    reinterpret_cast<void**>(const_cast<u32**>(&P))), "Map cont");
                std::memcpy(Saida.Contadores.data(), P, ContBytes);
                LeituraCont->Unmap(0, nullptr);
            }
            return Saida;
        }
    };
}

int main() try {
    std::cout << "Smile.GpuClassifyWarp\n";

    TestarDesfechos();
    TestarIndiceDeLote();
    TestarExatamenteUmaVez();

    const std::vector<u8> Bytecode = LerArquivo(SMILE_CLASSIFY_CANDIDATES_CSO);
    Check(!Bytecode.empty(), "a sonda compilada foi encontrada");
    if (Bytecode.empty()) {
        std::cerr << "  (esperado em " << SMILE_CLASSIFY_CANDIDATES_CSO << ")\n";
        return 1;
    }

    FSonda S;
    S.Construir(Bytecode);

    const auto Mats = Materiais(); const auto Meshs = Malhas();
    const auto Metas = Meta();     const auto Slot = Slots();

    FGpuClassifyInput In{};
    In.Slots = Slot; In.Meta = Metas; In.Materials = Mats; In.Meshes = Meshs;
    In.PoolCount = kPoolCount; In.BatchCapacity = GpuBatchSlotCount(kPoolCount);

    std::vector<u32> LotesCpu, ContCpu;
    ClassifyCandidates(In, LotesCpu, ContCpu);
    const FResultado Gpu = S.Executar(In);

    // O oraculo dos contadores e escrito a mao, e nao tirado da referencia CPU: as duas
    // implementacoes sao espelhos do mesmo raciocinio, e compara-las so entre si deixaria passar um
    // raciocinio errado nos dois lados.
    u32 Querido[GpuClassifyOutcomeCount]{};
    Querido[GpuClassifyIndirect]          = 3u; // slots 0, 1 e 2
    Querido[GpuClassifyNoSlot]            = 1u;
    Querido[GpuClassifyInvalidSlot]       = 2u; // linha vazia + slot fora da tabela
    Querido[GpuClassifyInvalidMaterialId] = 1u;
    Querido[GpuClassifyInvalidMeshId]     = 1u;
    Querido[GpuClassifyInvalidPoolId]     = 1u;
    Querido[GpuClassifyNotEligible]       = 4u; // two-sided, masked, blend, e o de dois termos
    Querido[GpuClassifyUnrepresentable]   = 1u;
    Querido[GpuClassifyOverflow]          = 0u;

    u32 SomaGpu = 0;
    for (u32 O = 0; O < GpuClassifyOutcomeCount; ++O) {
        CheckEq(Gpu.Contadores[O], Querido[O],
                std::string("contador ") + NomeDesfecho(O) + " na GPU");
        CheckEq(ContCpu[O], Querido[O], std::string("contador ") + NomeDesfecho(O) + " na CPU");
        SomaGpu += Gpu.Contadores[O];
    }
    CheckEq(SomaGpu, static_cast<u32>(Slot.size()),
            "na GPU tambem: um desfecho por candidato, nem mais nem menos");

    // Os lotes, elemento a elemento. O lote 0 tem DOIS comandos: sem ele, um shader que
    // sobrescrevesse em vez de somar passaria.
    const u32 QueridoLote[8]{ 2u, 1u, 0u, 0u, 0u, 0u, 0u, 0u };
    for (u32 B = 0; B < In.BatchCapacity; ++B) {
        CheckEq(Gpu.Lotes[B], QueridoLote[B], "lote " + std::to_string(B) + " na GPU");
        CheckEq(LotesCpu[B], QueridoLote[B], "lote " + std::to_string(B) + " na CPU");
    }
    // As faixas das classes 1..3 ficam em ZERO. Nao e redundancia com o laco acima: e o que prova
    // que a elegibilidade BARROU, e nao que o dado dessas classes faltou — a cena tem uma instancia
    // de cada uma delas, e as tres estao contadas em NotEligible.
    for (u32 C = 1; C < kGpuPipelineClassCount; ++C)
        for (u32 P = 0; P < kPoolCount; ++P)
            CheckEq(Gpu.Lotes[GpuBatchIndex({ C, P }, kPoolCount)], 0u,
                    "classe " + std::to_string(C) + " nao emite comando nesta fase");

    // Capacidade curta: o canario de alocacao. `BatchCapacity` viaja separada do `PoolCount` ate o
    // shader, e la os dois podem discordar sem que nada reclame — com capacidade 1, o comando do
    // pool 1 nao tem slot e tem de ser CONTADO, nao somado no slot errado.
    {
        FGpuClassifyInput Curta = In;
        Curta.BatchCapacity = 1u;
        std::vector<u32> LotesC, ContC;
        ClassifyCandidates(Curta, LotesC, ContC);
        const FResultado G = S.Executar(Curta);
        CheckEq(G.Contadores[GpuClassifyOverflow], 1u, "capacidade curta conta overflow na GPU");
        CheckEq(ContC[GpuClassifyOverflow], 1u, "capacidade curta conta overflow na CPU");
        CheckEq(G.Contadores[GpuClassifyIndirect], 2u, "e os outros dois comandos continuam");
        CheckEq(G.Lotes[0], 2u, "o lote que cabe recebe os dois");
        u32 Soma = 0;
        for (const u32 C : G.Contadores) Soma += C;
        CheckEq(Soma, static_cast<u32>(Slot.size()), "overflow tambem e UM desfecho, nao zero");
    }

    // Lista vazia: nenhum dispatch, nenhum contador. Sem este caso, um shader que lesse
    // `ExpandedSlots[0]` fora de guarda so apareceria numa cena sem candidatos.
    {
        FGpuClassifyInput Vazia = In;
        Vazia.Slots = {};
        const FResultado G = S.Executar(Vazia);
        u32 Soma = 0;
        for (const u32 C : G.Contadores) Soma += C;
        CheckEq(Soma, 0u, "lista vazia nao produz desfecho nenhum");
    }

    if (Failures == 0) {
        std::cout << "  " << Slot.size() << " candidatos classificados em WARP, todos OK\n";
        return 0;
    }
    std::cerr << "  " << Failures << " falha(s)\n";
    return 1;
} catch (const std::exception& E) {
    std::cerr << "Smile.GpuClassifyWarp: excecao -- " << E.what() << '\n';
    return 2;
}
