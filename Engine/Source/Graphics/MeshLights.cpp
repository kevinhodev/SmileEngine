#include "Smile/Graphics/MeshLights.h"
#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Core/Logger.h"
#include "Smile/Core/HResultCheck.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <cmath>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        // Mesmo criterio do FillInstanceGeo (DDGI.cpp): o que vale para o RT e o EmissiveStrength
        // JA escalado pelo RTEmissiveScale. Uma malha com RTEmissiveScale = 0 foi marcada pelo
        // artista como "brilha na tela mas nao ilumina" e nao pode virar mesh light — senao o
        // slider deixaria de significar o que promete.
        bool IsEmissiveForRT(const FMaterial& Material) {
            const MaterialConstants& MC = Material.Constants;
            const f32 Strength = MC.EmissiveStrength * MC.RTEmissiveScale;
            if (Strength <= 0.0f) return false;
            return MC.EmissiveFactor.X > 0.0f || MC.EmissiveFactor.Y > 0.0f ||
                   MC.EmissiveFactor.Z > 0.0f;
        }
    }

    void FMeshLights::Survey(const FScene& _Scene) {
        SceneStats = FStats{};

        for (const FRenderable& R : _Scene.Renderables()) {
            if (!R.Mesh || !R.Mesh->IsValid()) continue;
            const u32 Tris = R.Mesh->GetIndexCount() / 3u;
            ++SceneStats.TotalRenderables;
            SceneStats.TotalTriangles += Tris;

            if (!R.Visible || !R.Material || !IsEmissiveForRT(*R.Material)) continue;

            ++SceneStats.EmissiveMeshes;
            SceneStats.EmissiveTriangles += Tris;
            SceneStats.LargestMeshTris = std::max(SceneStats.LargestMeshTris, Tris);

            // Malha com mapa emissivo exige amostrar a textura por triangulo na extracao (o RTXDI
            // faz SampleGrad anisotropico com uma elipse inscrita no triangulo em UV, para obter a
            // radiancia MEDIA e nao um point sample do centro). Sem mapa, a radiancia e constante
            // e o triangulo sai direto do EmissiveFactor.
            if (R.Material->Constants.HasEmissiveMap) {
                ++SceneStats.MeshesWithMap;
                SceneStats.TrianglesWithMap += Tris;
            }
        }
    }

    void FMeshLights::LogSummary() const {
        const FStats& S = SceneStats;
        if (S.EmissiveTriangles == 0) {
            LogInfo("MeshLights: nenhuma geometria emissiva na cena (" +
                    std::to_string(S.TotalRenderables) + " renderables).");
            return;
        }

        const f64 Pct = S.TotalTriangles > 0
                      ? (100.0 * S.EmissiveTriangles / static_cast<f64>(S.TotalTriangles)) : 0.0;

        LogInfo("MeshLights: " + std::to_string(S.EmissiveTriangles) +
                " triangulos emissivos em " + std::to_string(S.EmissiveMeshes) + " malhas (" +
                std::to_string(static_cast<u32>(Pct + 0.5)) + "% dos " +
                std::to_string(S.TotalTriangles) + " triangulos da cena).");
        LogInfo("MeshLights: com mapa emissivo: " + std::to_string(S.TrianglesWithMap) +
                " triangulos em " + std::to_string(S.MeshesWithMap) +
                " malhas | maior malha isolada: " + std::to_string(S.LargestMeshTris) +
                " triangulos.");

        // A leitura da contagem. Os limiares vem da razao entre candidatas iniciais e tamanho do
        // pool: com M candidatas uniformes de N luzes, a chance de achar a dominante e 1-(1-1/N)^M,
        // que despenca com N. Ver a comparacao com RTXDI/Falcor na revisao do ReSTIR DI.
        if (S.EmissiveTriangles <= 512u) {
            LogInfo("MeshLights: faixa BAIXA — proposta uniforme deve bastar; alias table por "
                    "potencia fica como refinamento.");
        } else if (S.EmissiveTriangles <= 8192u) {
            LogInfo("MeshLights: faixa MEDIA — alias table por potencia vira obrigatoria "
                    "(uniforme perde a luz dominante na maioria dos frames).");
        } else {
            LogInfo("MeshLights: faixa ALTA — alias table por potencia MAIS ReGIR; nesta faixa o "
                    "pool de 64 slots por celula do ReGIR colapsa e precisa do presample "
                    "cooperativo antes de servir de proposta.");
        }
    }

    namespace {
        ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* _Device, u64 _Size,
                                            D3D12_HEAP_TYPE _HeapType,
                                            D3D12_RESOURCE_STATES _State,
                                            D3D12_RESOURCE_FLAGS _Flags) {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = _HeapType;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = _Size;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Desc.Flags            = _Flags;
            ComPtr<ID3D12Resource> Buffer;
            SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     _State, nullptr, IID_PPV_ARGS(&Buffer)));
            return Buffer;
        }
    }

    void FMeshLights::Initialize(ID3D12Device* _Device) {
        // Bindless ligado: a extracao le VB/IB e a textura emissiva por ResourceDescriptorHeap.
        ExtractPSO.Initialize(_Device, "MeshLightExtract.cs_6_6.cso", 2, 1, true);
        ConstantBuffer = CreateBuffer(_Device, sizeof(MeshLightConstants),
                                      D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                      D3D12_RESOURCE_FLAG_NONE);
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(ConstantBuffer->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));
        Initialized = true;
    }

    void FMeshLights::RecreatePSO(ID3D12Device* _Device) {
        if (!Initialized) return;
        ExtractPSO.Initialize(_Device, "MeshLightExtract.cs_6_6.cso", 2, 1, true);
    }

    FPassShaderStems FMeshLights::ShaderStems() const {
        static const char* const kStems[] = { "MeshLightExtract.cs" };
        return { kStems, 1 };
    }

    void FMeshLights::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (_Ctx.Device) RecreatePSO(_Ctx.Device);
    }

    D3D12_GPU_VIRTUAL_ADDRESS FMeshLights::CBAddr() const {
        return ConstantBuffer ? ConstantBuffer->GetGPUVirtualAddress() : 0;
    }

    void FMeshLights::Release(FTextureSRVHeap& _SRVHeap) {
        auto FreeSlot = [&](u32& Slot, u32 Count) {
            if (Slot != 0xFFFFFFFFu) { _SRVHeap.Free(Slot, Count); Slot = 0xFFFFFFFFu; }
        };
        FreeSlot(TaskSRV, 1);
        FreeSlot(LightsSRV, 1);
        FreeSlot(LightsUAV, 1);
        FreeSlot(AliasSRV, 1);
        FreeSlot(ExtractTable, 2);
        TaskBuffer.Reset();
        LightBuffer.Reset();
        ReadbackBuffer.Reset();
        if (AliasBuffer && MappedAlias) { AliasBuffer->Unmap(0, nullptr); MappedAlias = nullptr; }
        AliasBuffer.Reset();
        AliasReady      = false;
        ReadbackPending = false;
        ReadbackAge     = 0;
        LightState   = D3D12_RESOURCE_STATE_COMMON;
        NumTasks     = 0;
        NumTriangles = 0;
        Ready        = false;
        Dirty        = false;
    }

    void FMeshLights::SetupForScene(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                    const FScene& _Scene, u32 _InstanceSlot) {
        Release(_SRVHeap);
        Survey(_Scene);
        LogSummary();
        if (!Initialized || _InstanceSlot == 0xFFFFFFFFu) return;

        // Uma task por malha emissiva, em ordem crescente de LightOffset e sem buracos — a busca
        // binaria do shader depende dessas duas propriedades.
        std::vector<FMeshLightTaskGPU> Tasks;
        Tasks.reserve(SceneStats.EmissiveMeshes);
        u32 Offset = 0;
        const std::vector<FRenderable>& List = _Scene.Renderables();
        for (u32 i = 0; i < static_cast<u32>(List.size()); ++i) {
            const FRenderable& R = List[i];
            if (!R.Mesh || !R.Mesh->IsValid() || !R.Visible || !R.Material) continue;
            if (!IsEmissiveForRT(*R.Material)) continue;

            FMeshLightTaskGPU T{};
            T.InstanceIndex = i;
            T.TriangleCount = R.Mesh->GetIndexCount() / 3u;
            T.LightOffset   = Offset;
            // Transposta, igual ao que o RaytracingScene entrega ao TLAS: as 3 primeiras linhas
            // da Mat44 transposta sao a matriz 3x4 linha-maior.
            const Mat44 M = R.Transform.Matrix().GetTransposed();
            T.Row0 = { M.M[0][0], M.M[0][1], M.M[0][2], M.M[0][3] };
            T.Row1 = { M.M[1][0], M.M[1][1], M.M[1][2], M.M[1][3] };
            T.Row2 = { M.M[2][0], M.M[2][1], M.M[2][2], M.M[2][3] };
            Offset += T.TriangleCount;
            Tasks.push_back(T);
        }
        NumTasks     = static_cast<u32>(Tasks.size());
        NumTriangles = Offset;

        // Aloca no minimo 1 elemento mesmo sem geometria emissiva. Custa 96 bytes e elimina um caso
        // de borda inteiro: o ReSTIR DI liga este SRV na tabela dele SEMPRE, e um descritor nulo ali
        // seria pior que um buffer vazio que ninguem le (a contagem no CB e que gateia o acesso).
        const u32 TaskElems  = std::max(NumTasks, 1u);
        const u32 LightElems = std::max(NumTriangles, 1u);

        // Upload heap e escrito UMA vez, aqui. SetupForScene roda depois de um Flush da fila
        // (SceneLoader), entao nao ha dispatch em voo lendo este buffer.
        TaskBuffer = CreateBuffer(_Device, sizeof(FMeshLightTaskGPU) * TaskElems,
                                  D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                  D3D12_RESOURCE_FLAG_NONE);
        void* Mapped = nullptr;
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(TaskBuffer->Map(0, &NoRead, &Mapped));
        std::memset(Mapped, 0, sizeof(FMeshLightTaskGPU) * TaskElems);
        if (NumTasks > 0)
            std::memcpy(Mapped, Tasks.data(), sizeof(FMeshLightTaskGPU) * NumTasks);
        TaskBuffer->Unmap(0, nullptr);

        LightBuffer = CreateBuffer(_Device, sizeof(FTriangleLightGPU) * LightElems,
                                   D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        VramTracker::Register(LightBuffer.Get(), EVramCategory::GI, "Mesh lights · triangulos");
        LightState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                     = DXGI_FORMAT_UNKNOWN;
        Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Buffer.NumElements         = TaskElems;
        Srv.Buffer.StructureByteStride = sizeof(FMeshLightTaskGPU);
        TaskSRV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, TaskBuffer.Get(), Srv, TaskSRV);

        Srv.Buffer.NumElements         = LightElems;
        Srv.Buffer.StructureByteStride = sizeof(FTriangleLightGPU);
        LightsSRV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, LightBuffer.Get(), Srv, LightsSRV);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format                     = DXGI_FORMAT_UNKNOWN;
        Uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.NumElements         = LightElems;
        Uav.Buffer.StructureByteStride = sizeof(FTriangleLightGPU);
        LightsUAV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateUAV(_Device, LightBuffer.Get(), Uav, LightsUAV);

        ReadbackBuffer = CreateBuffer(_Device, sizeof(FTriangleLightGPU) * LightElems,
                                      D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_FLAG_NONE);
        AliasBuffer = CreateBuffer(_Device, sizeof(FMeshLightAliasGPU) * LightElems,
                                   D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                   D3D12_RESOURCE_FLAG_NONE);
        D3D12_RANGE NoReadAlias{ 0, 0 };
        SMILE_HR(AliasBuffer->Map(0, &NoReadAlias, reinterpret_cast<void**>(&MappedAlias)));
        std::memset(MappedAlias, 0, sizeof(FMeshLightAliasGPU) * LightElems);

        Srv.Buffer.NumElements         = LightElems;
        Srv.Buffer.StructureByteStride = sizeof(FMeshLightAliasGPU);
        AliasSRV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, AliasBuffer.Get(), Srv, AliasSRV);

        ExtractTable = _SRVHeap.Allocate(2);
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(ExtractTable);
        D3D12_CPU_DESCRIPTOR_HANDLE Src[2] = {
            _SRVHeap.CpuHandleStaging(TaskSRV),
            _SRVHeap.CpuHandleStaging(_InstanceSlot),
        };
        UINT DstCount = 2; UINT Ones[2] = { 1, 1 };
        _Device->CopyDescriptors(1, &Dst, &DstCount, 2, Src, Ones,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        MeshLightConstants CPU{};
        CPU.NumTasks     = NumTasks;
        CPU.NumTriangles = NumTriangles;
        std::memcpy(MappedCB, &CPU, sizeof(CPU));

        Ready = true;
        Dirty = NumTriangles > 0;
    }

    // Vose: monta a tabela em O(N). Divide as entradas entre as que ficaram ABAIXO e ACIMA da
    // media e vai casando uma de cada, ate toda entrada ter no maximo um alias. O resultado
    // amostra proporcional ao fluxo com dois numeros aleatorios e uma leitura.
    void FMeshLights::BuildAliasTable() {
        if (!MappedAlias || NumTriangles == 0 || !ReadbackBuffer) return;

        const FTriangleLightGPU* Src = nullptr;
        D3D12_RANGE ReadAll{ 0, sizeof(FTriangleLightGPU) * NumTriangles };
        if (FAILED(ReadbackBuffer->Map(0, &ReadAll, reinterpret_cast<void**>(
                       const_cast<FTriangleLightGPU**>(&Src)))) || !Src)
            return;

        const u32 N = NumTriangles;
        std::vector<f64> P(N);
        f64 Total = 0.0;
        for (u32 i = 0; i < N; ++i) {
            const f32 F = Src[i].Flux;
            P[i]   = (F > 0.0f && std::isfinite(F)) ? static_cast<f64>(F) : 0.0;
            Total += P[i];
        }

        D3D12_RANGE NoWrite{ 0, 0 };
        ReadbackBuffer->Unmap(0, &NoWrite);

        FMeshLightAliasGPU* Dst = reinterpret_cast<FMeshLightAliasGPU*>(MappedAlias);
        if (Total <= 0.0) {
            // Nenhum triangulo com fluxo (tudo com RTEmissiveScale 0, por exemplo): cai para
            // uniforme em vez de deixar a tabela zerada, que devolveria pdf 0 e mataria a amostra.
            const f32 Uniform = 1.0f / static_cast<f32>(N);
            for (u32 i = 0; i < N; ++i) Dst[i] = { 1.0f, i, Uniform, Uniform };
            AliasReady = true;
            LogInfo("MeshLights: alias table uniforme (fluxo total zero).");
            return;
        }

        std::vector<f32> Prob(N);
        std::vector<u32> Small, Large;
        Small.reserve(N); Large.reserve(N);
        for (u32 i = 0; i < N; ++i) {
            P[i] /= Total;                       // p(i) normalizado, o que o shader precisa
            const f64 Scaled = P[i] * N;         // media 1 apos a escala
            Prob[i] = static_cast<f32>(Scaled);
            (Scaled < 1.0 ? Small : Large).push_back(i);
        }

        std::vector<u32> Alias(N);
        for (u32 i = 0; i < N; ++i) Alias[i] = i;
        while (!Small.empty() && !Large.empty()) {
            const u32 s = Small.back(); Small.pop_back();
            const u32 l = Large.back(); Large.pop_back();
            Alias[s] = l;
            Prob[l]  = static_cast<f32>((static_cast<f64>(Prob[l]) + Prob[s]) - 1.0);
            (Prob[l] < 1.0f ? Small : Large).push_back(l);
        }
        // Resto por erro de arredondamento: fica com probabilidade cheia na propria entrada.
        for (u32 i : Large) Prob[i] = 1.0f;
        for (u32 i : Small) Prob[i] = 1.0f;

        for (u32 i = 0; i < N; ++i) {
            Dst[i].Threshold = Prob[i];
            Dst[i].Alias     = Alias[i];
            Dst[i].ProbSelf  = static_cast<f32>(P[i]);
            Dst[i].ProbAlias = static_cast<f32>(P[Alias[i]]);
        }

        AliasReady = true;
        LogInfo("MeshLights: alias table pronta para " + std::to_string(N) +
                " triangulos (fluxo total " + std::to_string(Total) + ").");
    }

    void FMeshLights::Record(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;

        // Readback diferido: espera a fila ciclar em vez de travar. Enquanto nao chega, LightCount()
        // devolve 0 e o DI simplesmente nao ve mesh light — melhor que ver com proposta uniforme.
        if (ReadbackPending) {
            if (++ReadbackAge >= FCommandQueue::kFramesInFlight + 1u) {
                ReadbackPending = false;
                BuildAliasTable();
            }
        }
        if (!Dirty) return;

        if (LightState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = LightBuffer.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = LightState;
            B.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            _CL->ResourceBarrier(1, &B);
            LightState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        ExtractPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ExtractTable));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(LightsUAV));
        _CL->Dispatch((NumTriangles + 63u) / 64u, 1, 1);

        auto Transition = [&](D3D12_RESOURCE_STATES After) {
            if (LightState == After) return;
            D3D12_RESOURCE_BARRIER B{};
            B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource   = LightBuffer.Get();
            B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore = LightState;
            B.Transition.StateAfter  = After;
            _CL->ResourceBarrier(1, &B);
            LightState = After;
        };

        // Copia o resultado para o readback: e dele que sai o fluxo por triangulo que alimenta a
        // alias table. Custa uma copia so quando a cena muda, nao por frame.
        Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        _CL->CopyResource(ReadbackBuffer.Get(), LightBuffer.Get());
        Transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        ReadbackPending = true;
        ReadbackAge     = 0;
        AliasReady      = false;

        // Estatico: sem isto a extracao rodaria todo frame reconstruindo o mesmo dado.
        Dirty = false;
    }
}
