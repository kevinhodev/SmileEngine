#include "Smile/Graphics/Lighting/MeshLights.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
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
        // Mesmo criterio do FillInstanceGeo (RaytracingScene.cpp): o que vale para o RT e o EmissiveStrength
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

        // fp16 -> f32 para ler as arestas do readback. Existe aqui e nao no Math.h porque este e
        // o unico consumidor CPU do empacotamento do MeshLightCommon.hlsli; promover a utilitario
        // geral criaria uma segunda convencao de half na engine sem cliente que a justifique.
        //
        // Subnormais sao normalizados, e NAO achatados em zero. Achatar parece inofensivo e nao e:
        // o maior subnormal de half vale 2^-14 ~= 6,1e-5 (e nao ~6e-8, que e o MENOR, 2^-24). Um
        // triangulo com as arestas nessa ordem tem |cross| ~= 3,6e-9, muito acima do 1e-12 que o
        // DI_SampleTriangleLight testa: o shader o aceita, e achatar as componentes faria o
        // contador da CPU marca-lo como degenerado. Seria justamente a divergencia entre as duas
        // nocoes de degenerado que o TriangleCrossLength existe para nao ter.
        f32 HalfToFloat(u16 H) {
            const u32 Sign = static_cast<u32>(H & 0x8000u) << 16;
            const u32 Exp  = (H >> 10) & 0x1Fu;
            u32       Mant = H & 0x03FFu;
            u32 Bits;
            if (Exp == 0u) {
                if (Mant == 0u) {
                    Bits = Sign; // +-0
                } else {
                    // Desloca ate o bit implicito aparecer. Valor = m * 2^-24; com Shift
                    // deslocamentos o expoente sem vies fica -14 - Shift, ou seja 113 - Shift
                    // depois do vies de 127.
                    u32 Shift = 0;
                    while ((Mant & 0x0400u) == 0u) { Mant <<= 1; ++Shift; }
                    Mant &= 0x03FFu;
                    Bits = Sign | ((113u - Shift) << 23) | (Mant << 13);
                }
            } else if (Exp == 31u) {
                Bits = Sign | 0x7F800000u | (Mant << 13);     // Inf/NaN
            } else {
                Bits = Sign | ((Exp + 112u) << 23) | (Mant << 13); // 127 - 15
            }
            f32 Out;
            std::memcpy(&Out, &Bits, sizeof(Out));
            return Out;
        }

        // |cross(e0, e1)| do triangulo extraido — o dobro da area. Devolve exatamente a grandeza
        // que o DI_SampleTriangleLight testa contra 1e-12, para o contador de degenerados contar
        // os triangulos que o SHADER rejeita e nao uma nocao paralela de degenerado.
        f32 TriangleCrossLength(const FTriangleLightGPU& T) {
            const f32 E0x = HalfToFloat(static_cast<u16>(T.Edges0 & 0xFFFFu));
            const f32 E0y = HalfToFloat(static_cast<u16>(T.Edges1 & 0xFFFFu));
            const f32 E0z = HalfToFloat(static_cast<u16>(T.Edges2 & 0xFFFFu));
            const f32 E1x = HalfToFloat(static_cast<u16>(T.Edges0 >> 16));
            const f32 E1y = HalfToFloat(static_cast<u16>(T.Edges1 >> 16));
            const f32 E1z = HalfToFloat(static_cast<u16>(T.Edges2 >> 16));
            const f32 Cx = E0y * E1z - E0z * E1y;
            const f32 Cy = E0z * E1x - E0x * E1z;
            const f32 Cz = E0x * E1y - E0y * E1x;
            return std::sqrt(Cx * Cx + Cy * Cy + Cz * Cz);
        }
    }

    namespace {
        // Todos os buffers de triangulo/alias sao dimensionados pelo mesmo LightElems do
        // SetupForScene: no minimo um elemento, mesmo sem geometria emissiva.
        u64 PayloadBytes(const Microsoft::WRL::ComPtr<ID3D12Resource>& Res, u32 NumTriangles,
                         size_t Stride) {
            if (!Res) return 0;
            return static_cast<u64>(std::max(NumTriangles, 1u)) * Stride;
        }

        // Footprint de um BUFFER: o payload arredondado para cima no alinhamento de alocacao de
        // recurso do D3D12 (64 KiB). Para buffer isso e exatamente o que o
        // GetResourceAllocationInfo devolveria, e por ser deterministico dispensa guardar o
        // resultado por recurso na criacao.
        u64 AlignedFootprint(u64 Payload) {
            if (Payload == 0) return 0;
            constexpr u64 kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64 KiB
            return (Payload + kAlign - 1u) & ~(kAlign - 1u);
        }
    }

    u64 FMeshLights::TrianglePayloadBytes() const {
        return PayloadBytes(LightBuffer, NumTriangles, sizeof(FTriangleLightGPU));
    }

    u64 FMeshLights::TriangleCompactPayloadBytes() const {
        return PayloadBytes(CompactLightBuffer, NumTriangles, sizeof(FTriangleLightGPU));
    }

    u64 FMeshLights::TriangleStagingPayloadBytes() const {
        return PayloadBytes(CompactUploadBuffer, NumTriangles, sizeof(FTriangleLightGPU));
    }

    u64 FMeshLights::TriangleReadbackPayloadBytes() const {
        return PayloadBytes(ReadbackBuffer, NumTriangles, sizeof(FTriangleLightGPU));
    }

    // Só o que ocupa VRAM: os dois DEFAULT de triangulo mais a alias em DEFAULT. O staging e o
    // readback moram em system memory e por isso ficam fora — somá-los aqui inflaria o orçamento
    // de VRAM com memória que não é VRAM, que é exatamente o erro que este bloco existe p/ evitar.
    //
    // Soma de FOOTPRINTS ALINHADOS, e não de payloads: um pool de um triângulo tem 32 B de payload
    // e ocupa 64 KiB. Somar payload aqui subestimaria a VRAM por três ordens de grandeza em cena
    // pequena — e o campo se chama VRAM, então tem de descrever VRAM.
    u64 FMeshLights::VramBytes() const {
        return AlignedFootprint(TrianglePayloadBytes()) +
               AlignedFootprint(TriangleCompactPayloadBytes()) +
               AlignedFootprint(AliasDefaultPayloadBytes());
    }

    u64 FMeshLights::AliasUploadPayloadBytes() const {
        return PayloadBytes(AliasBuffer, NumTriangles, sizeof(FMeshLightAliasGPU));
    }

    u64 FMeshLights::AliasDefaultPayloadBytes() const {
        return PayloadBytes(AliasDefaultBuffer, NumTriangles, sizeof(FMeshLightAliasGPU));
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

    void FMeshLights::Initialize(ID3D12Device* _Device) {
        // Bindless ligado: a extracao le VB/IB e a textura emissiva por ResourceDescriptorHeap.
        ExtractPSO.Initialize(_Device, "MeshLightExtract.cs_6_6.cso", 2, 1, true);
        const GpuResources::FUploadBuffer Upload =
            GpuResources::CreateUploadBuffer(_Device, sizeof(MeshLightConstants));
        ConstantBuffer = Upload.Resource;
        MappedCB       = Upload.Mapped;
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
        FreeSlot(AliasDefaultSRV, 1);
        FreeSlot(CompactLightsSRV, 1);
        FreeSlot(ExtractTable, 2);
        TaskBuffer.Reset();
        MappedTasks = nullptr;
        CpuTasks.clear();
        LightBuffer.Reset();
        ReadbackBuffer.Reset();
        if (AliasBuffer && MappedAlias) { AliasBuffer->Unmap(0, nullptr); MappedAlias = nullptr; }
        AliasBuffer.Reset();
        AliasDefaultBuffer.Reset();
        CompactLightBuffer.Reset();
        CompactUploadBuffer.Reset();
        MappedCompact      = nullptr;
        CompactLightState  = D3D12_RESOURCE_STATE_COMMON;
        CompactCopyPending = false;
        CompactApplied     = false;
        NumSamplable       = 0;
        DomainPublished      = false;
        PendingDomainPublish = false;
        AliasDefaultState  = D3D12_RESOURCE_STATE_COMMON;
        // O PEDIDO sobrevive ao rebuild de cena: ele e configuracao do operador, nao estado do
        // recurso. O que morre e a copia e o vinculo, que descrevem buffers que deixaram de existir.
        AliasDefaultCopied = false;
        AliasCopyPending   = false;
        AliasReady      = false;
        DistStats       = FDistributionStats{};
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
        CpuTasks.clear();
        CpuTasks.reserve(SceneStats.EmissiveMeshes);
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
            CpuTasks.push_back(T);
        }
        NumTasks     = static_cast<u32>(CpuTasks.size());
        NumTriangles = Offset;

        // Aloca no minimo 1 elemento mesmo sem geometria emissiva. Custa 96 bytes e elimina um caso
        // de borda inteiro: o ReSTIR DI liga este SRV na tabela dele SEMPRE, e um descritor nulo ali
        // seria pior que um buffer vazio que ninguem le (a contagem no CB e que gateia o acesso).
        const u32 TaskElems  = std::max(NumTasks, 1u);
        const u32 LightElems = std::max(NumTriangles, 1u);

        // Upload heap e escrito UMA vez, aqui. SetupForScene roda depois de um Flush da fila
        // (SceneLoader), entao nao ha dispatch em voo lendo este buffer.
        // O par Map/Unmap explicito sumiu junto com a criacao a mao: o buffer da fabrica ja
        // vem mapeado de forma persistente, que e o normal para upload heap.
        const GpuResources::FUploadBuffer TaskUpload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(FMeshLightTaskGPU) * TaskElems, 1, false);
        TaskBuffer  = TaskUpload.Resource;
        MappedTasks = TaskUpload.Mapped;
        std::memset(TaskUpload.Mapped, 0, sizeof(FMeshLightTaskGPU) * TaskElems);
        if (NumTasks > 0)
            std::memcpy(TaskUpload.Mapped, CpuTasks.data(), sizeof(FMeshLightTaskGPU) * NumTasks);

        LightBuffer = GpuResources::CreateBuffer(
            _Device, sizeof(FTriangleLightGPU) * LightElems,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
            EVramCategory::GI, "Mesh lights · triangulos");
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

        ReadbackBuffer = GpuResources::CreateReadbackBuffer(
            _Device, sizeof(FTriangleLightGPU) * LightElems);

        const GpuResources::FUploadBuffer Alias = GpuResources::CreateUploadBuffer(
            _Device, sizeof(FMeshLightAliasGPU) * LightElems, 1, false);
        AliasBuffer = Alias.Resource;
        MappedAlias = Alias.Mapped;
        std::memset(MappedAlias, 0, sizeof(FMeshLightAliasGPU) * LightElems);

        Srv.Buffer.NumElements         = LightElems;
        Srv.Buffer.StructureByteStride = sizeof(FMeshLightAliasGPU);
        AliasSRV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, AliasBuffer.Get(), Srv, AliasSRV);

        // Copia em DEFAULT heap, alocada SEMPRE — inclusive com o toggle desligado. O A/B da
        // Fase 0.5 varia so o descritor que o Pass A le; alocar junto com o toggle faria o braco
        // ligado pagar tambem a pressao de memoria, e a medida atribuiria a localizacao um custo
        // que era de alocacao.
        AliasDefaultBuffer = GpuResources::CreateBuffer(
            _Device, sizeof(FMeshLightAliasGPU) * LightElems,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON,
            EVramCategory::GI, "Mesh lights · alias (default heap)");
        AliasDefaultState = D3D12_RESOURCE_STATE_COMMON;
        AliasDefaultSRV   = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, AliasDefaultBuffer.Get(), Srv, AliasDefaultSRV);

        // Copia COMPACTA dos triangulos, em VRAM, e o staging dela. Dimensionados pelo PIOR caso
        // (todos os triangulos com fluxo) para o SRV nunca precisar ser recriado quando o conteudo
        // da cena mudar: quem varia e a CONTAGEM publicada, nao a alocacao.
        const GpuResources::FUploadBuffer CompactUpload = GpuResources::CreateUploadBuffer(
            _Device, sizeof(FTriangleLightGPU) * LightElems, 1, false);
        CompactUploadBuffer = CompactUpload.Resource;
        MappedCompact       = CompactUpload.Mapped;
        std::memset(MappedCompact, 0, sizeof(FTriangleLightGPU) * LightElems);

        CompactLightBuffer = GpuResources::CreateBuffer(
            _Device, sizeof(FTriangleLightGPU) * LightElems,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON,
            EVramCategory::GI, "Mesh lights · triangulos compactos");
        CompactLightState = D3D12_RESOURCE_STATE_COMMON;

        Srv.Buffer.NumElements         = LightElems;
        Srv.Buffer.StructureByteStride = sizeof(FTriangleLightGPU);
        CompactLightsSRV = _SRVHeap.Allocate(1);
        _SRVHeap.CreateSRV(_Device, CompactLightBuffer.Get(), Srv, CompactLightsSRV);

        NumSamplable   = NumTriangles;
        CompactApplied = false;

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

    FMeshLights::ETransformRefresh FMeshLights::RefreshTransforms(const FScene& _Scene) {
        // NumTasks == 0 NAO sai cedo: cena sem malha emissiva alguma precisa continuar
        // caminhando, senao a primeira que ficasse visivel nunca seria detectada (o walk abaixo
        // devolve SetChanged no primeiro emissivo que aparece alem do fim da lista).
        if (!Ready || !MappedTasks) return ETransformRefresh::Unchanged;

        // O buffer de tasks e UPLOAD e a extracao le direto dele. Enquanto houver dispatch que
        // possa estar em voo, escrever aqui seria corrida com a GPU. Dirty = a extracao ainda vai
        // ser gravada; ReadbackPending = ja foi, e o contador de kFramesInFlight+1 (o mesmo que o
        // Record usa) ainda nao provou que ela terminou. Com os dois em falso ninguem le o buffer.
        if (Dirty || ReadbackPending) return ETransformRefresh::Busy;

        // MESMO filtro e MESMA ordem do SetupForScene. Se divergir, o LightOffset de cada task
        // deixa de casar com o buffer de triangulos e a busca binaria do shader passa a resolver
        // o triangulo errado.
        //
        // Confere e escreve na COPIA DE CPU, comitando por cima do mapeado so no fim. Dois
        // motivos: o mapeado e write-combined e le-lo custa ordens de grandeza mais que memoria
        // normal, e escrever durante a caminhada deixaria o buffer meio remendado se a
        // divergencia aparecesse no meio dela.
        if (CpuTasks.size() != NumTasks) return ETransformRefresh::SetChanged;
        const std::vector<FRenderable>& List = _Scene.Renderables();
        u32  Slot    = 0;
        bool Changed = false;
        for (u32 i = 0; i < static_cast<u32>(List.size()); ++i) {
            const FRenderable& R = List[i];
            if (!R.Mesh || !R.Mesh->IsValid() || !R.Visible || !R.Material) continue;
            if (!IsEmissiveForRT(*R.Material)) continue;
            if (Slot >= NumTasks || CpuTasks[Slot].InstanceIndex != i)
                return ETransformRefresh::SetChanged;

            // So as 3 linhas mudam: InstanceIndex, TriangleCount e LightOffset descrevem o
            // PARTICIONAMENTO, e ele e justamente o que acabou de ser conferido como intacto.
            const Mat44 M = R.Transform.Matrix().GetTransposed();
            const Vec4 Row0{ M.M[0][0], M.M[0][1], M.M[0][2], M.M[0][3] };
            const Vec4 Row1{ M.M[1][0], M.M[1][1], M.M[1][2], M.M[1][3] };
            const Vec4 Row2{ M.M[2][0], M.M[2][1], M.M[2][2], M.M[2][3] };

            // Comparacao EXATA, e nao por epsilon: a pergunta aqui e "alguem mexeu?", nao "as
            // matrizes se parecem?". A TransformsVersion e GLOBAL, entao mover um objeto nao
            // emissivo tambem chega ate aqui — e a malha emissiva parada recomputa a matriz das
            // MESMAS entradas pelo MESMO caminho, produzindo bit a bit o mesmo resultado. Sem
            // isto, arrastar qualquer objeto da cena fecharia o pool e refaria extracao + alias
            // table sem nada ter mudado, apagando as mesh lights por alguns frames a cada gesto.
            FMeshLightTaskGPU& T = CpuTasks[Slot];
            if (T.Row0.X != Row0.X || T.Row0.Y != Row0.Y || T.Row0.Z != Row0.Z || T.Row0.W != Row0.W ||
                T.Row1.X != Row1.X || T.Row1.Y != Row1.Y || T.Row1.Z != Row1.Z || T.Row1.W != Row1.W ||
                T.Row2.X != Row2.X || T.Row2.Y != Row2.Y || T.Row2.Z != Row2.Z || T.Row2.W != Row2.W) {
                T.Row0 = Row0; T.Row1 = Row1; T.Row2 = Row2;
                Changed = true;
            }
            ++Slot;
        }
        if (Slot != NumTasks) return ETransformRefresh::SetChanged;
        if (!Changed) return ETransformRefresh::Unchanged;

        std::memcpy(MappedTasks, CpuTasks.data(), sizeof(FMeshLightTaskGPU) * NumTasks);
        // A contagem de triangulos nao mudou (mesmo conjunto de malhas), entao o CB continua
        // valido: so o dado geometrico foi reescrito. Sujo re-dispara a extracao, que refaz
        // area e fluxo por triangulo e, por consequencia, a alias table.
        Dirty = true;
        return ETransformRefresh::Applied;
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

        // Primeira passada: estatisticas e o SUPORTE POSITIVO. `Support` guarda os indices no
        // dominio original; ele vira o dominio compacto se a compactacao estiver ligada.
        std::vector<u32> Support;
        Support.reserve(NumTriangles);
        f64 Total = 0.0;
        DistStats = FDistributionStats{};
        DistStats.Triangles = NumTriangles;
        for (u32 i = 0; i < NumTriangles; ++i) {
            const f32 F = Src[i].Flux;
            // Mesma ORDEM de rejeicao do DI_SampleTriangleLight: corrompido, depois degenerado,
            // depois sem energia. Classificar fora dessa ordem faria um triangulo degenerado E sem
            // radiancia aparecer como conteudo, escondendo o defeito de geometria atras dele.
            if (!std::isfinite(F))                        ++DistStats.NonFinite;
            else if (TriangleCrossLength(Src[i]) < 1e-12f) ++DistStats.Degenerate;
            else if (F <= 0.0f)                            ++DistStats.ZeroFlux;

            if (F > 0.0f && std::isfinite(F)) {
                Support.push_back(i);
                Total += static_cast<f64>(F);
            }
        }
        DistStats.TotalFlux = Total;

        // Compacta so quando ha suporte positivo. Com fluxo total zero o caminho antigo (tabela
        // uniforme sobre TODOS os triangulos) e preservado inteiro: mudar aquele caso aqui seria
        // uma segunda alteracao de comportamento escondida dentro desta.
        CompactApplied = CompactRequested && Total > 0.0 && MappedCompact != nullptr &&
                         !Support.empty();
        NumSamplable   = CompactApplied ? static_cast<u32>(Support.size()) : NumTriangles;
        const u32 N    = NumSamplable;

        // P e construido sobre o dominio EFETIVAMENTE AMOSTRADO, e o tamanho dele tem de ser N.
        //
        // Aqui morava um bug: P era preenchido so com o suporte positivo, mas N caia para
        // NumTriangles quando a compactacao estava desligada — e o laco de Vose abaixo indexava
        // P[i] alem do fim do vetor. O braco de CONTROLE do A/B construia a tabela a partir de
        // memoria de heap arbitraria, e so foi pego porque a imagem dos dois bracos foi comparada:
        // o compactado saiu 4% mais claro, contra um piso de ruido de 0,02%.
        std::vector<f64> P(N);
        if (CompactApplied) {
            for (u32 k = 0; k < N; ++k) P[k] = static_cast<f64>(Src[Support[k]].Flux);
        } else {
            for (u32 i = 0; i < N; ++i) {
                const f32 F = Src[i].Flux;
                P[i] = (F > 0.0f && std::isfinite(F)) ? static_cast<f64>(F) : 0.0;
            }
        }

        if (CompactApplied) {
            // Os 32 bytes do triangulo viajam inteiros: o dominio compacto nao precisa de mapa de
            // volta para o indice original, porque o reservoir guarda o indice COMPACTO e o
            // triangulo que ele aponta ja tem tudo que o Pass A e o Pass B leem.
            FTriangleLightGPU* CDst = reinterpret_cast<FTriangleLightGPU*>(MappedCompact);
            for (u32 k = 0; k < N; ++k) CDst[k] = Src[Support[k]];
            CompactCopyPending = true;
        } else if (!CompactRequested || Total <= 0.0) {
            // Sem compactacao o Pass A amostra o buffer original, mas o SRV compacto continua
            // ligado na tabela de trace — entao ele precisa receber os mesmos dados.
            if (MappedCompact) {
                std::memcpy(MappedCompact, Src, sizeof(FTriangleLightGPU) * NumTriangles);
                CompactCopyPending = true;
            }
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
            AliasCopyPending = true;
            DistStats.UniformFallback = true;
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
        // A copia para o DEFAULT heap fica pendente ate o proximo Record. O conteudo mudou, entao
        // a copia anterior — se existia — descreve uma tabela que nao vale mais.
        AliasCopyPending = true;
        LogInfo("MeshLights: alias table pronta para " + std::to_string(N) +
                " triangulos (fluxo total " + std::to_string(Total) + ").");
        if (CompactApplied) {
            LogInfo("MeshLights: dominio compactado de " + std::to_string(NumTriangles) +
                    " para " + std::to_string(N) + " triangulos (" +
                    std::to_string(static_cast<u32>(100.0 * N / NumTriangles + 0.5)) +
                    "% do original).");
        }
        const u32 Rejected = DistStats.Degenerate + DistStats.ZeroFlux + DistStats.NonFinite;
        if (Rejected > 0) {
            // So quando existe: numa cena limpa esta linha nao aparece, e a ausencia dela e
            // informacao. Aparecendo, ela diz qual dos tres problemas comprar.
            LogInfo("MeshLights: " + std::to_string(Rejected) + " triangulos sem contribuicao (" +
                    std::to_string(DistStats.Degenerate) + " degenerados, " +
                    std::to_string(DistStats.ZeroFlux) + " com radiancia zero, " +
                    std::to_string(DistStats.NonFinite) + " com fluxo invalido).");
        }
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

        // Copia UPLOAD -> DEFAULT da alias table. FORA do gate de Dirty logo abaixo: quem acabou de
        // produzir a tabela foi o BuildAliasTable acima, num frame em que a cena ja nao esta suja —
        // dentro do gate, a copia nunca aconteceria.
        //
        // Os bytes sao os MESMOS: uma copia do recurso inteiro, sem reconstruir a tabela. E o que
        // faz o A/B medir localizacao e nada mais; reconstruir dos dois lados abriria espaco para
        // as duas distribuicoes divergirem por arredondamento.
        //
        // O upload heap fica em GENERIC_READ permanentemente (exigencia do tipo de heap), entao so
        // o destino transiciona. Este passe roda ANTES do ReSTIR DI no mesmo command list, e a
        // barreira abaixo e o que garante que o Pass A do mesmo frame ja leia o conteudo copiado.
        if (AliasCopyPending && AliasDefaultBuffer && AliasBuffer) {
            auto ToState = [&](D3D12_RESOURCE_STATES After) {
                if (AliasDefaultState == After) return;
                D3D12_RESOURCE_BARRIER B{};
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.pResource   = AliasDefaultBuffer.Get();
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateBefore = AliasDefaultState;
                B.Transition.StateAfter  = After;
                _CL->ResourceBarrier(1, &B);
                AliasDefaultState = After;
            };
            ToState(D3D12_RESOURCE_STATE_COPY_DEST);
            _CL->CopyResource(AliasDefaultBuffer.Get(), AliasBuffer.Get());
            ToState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            AliasCopyPending   = false;
            AliasDefaultCopied = true;
        }

        // Triangulos compactados: mesma janela e mesmo raciocinio da copia acima. Os dois buffers
        // sao publicados juntos porque o indice do reservoir e o da alias vivem no MESMO dominio —
        // subir um sem o outro faria a probabilidade apontar para o triangulo errado.
        if (CompactCopyPending && CompactLightBuffer && CompactUploadBuffer) {
            auto ToState = [&](D3D12_RESOURCE_STATES After) {
                if (CompactLightState == After) return;
                D3D12_RESOURCE_BARRIER B{};
                B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                B.Transition.pResource   = CompactLightBuffer.Get();
                B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                B.Transition.StateBefore = CompactLightState;
                B.Transition.StateAfter  = After;
                _CL->ResourceBarrier(1, &B);
                CompactLightState = After;
            };
            ToState(D3D12_RESOURCE_STATE_COPY_DEST);
            _CL->CopyResource(CompactLightBuffer.Get(), CompactUploadBuffer.Get());
            ToState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CompactCopyPending = false;
        }

        // Com os dois buffers no lugar, o dominio novo esta pronto — mas so PENDENTE. Quem o abre e
        // o Renderer, depois de limpar o historico: o indice do reservoir pode ter mudado de
        // significado, e servi-lo contra o dominio novo apontaria para outro triangulo.
        if (AliasReady && AliasDefaultCopied && !CompactCopyPending && !DomainPublished)
            PendingDomainPublish = true;

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
        // Fecha o pool enquanto a tabela e reconstruida. Sem isto, o dominio ANTIGO continuaria
        // publicado durante o rebuild e o `effective` do manifesto descreveria um regime que a
        // proxima tabela pode nem ter.
        DomainPublished      = false;
        PendingDomainPublish = false;
        // Junto com o AliasReady, e nao so no Release: durante o rebuild a distribuicao publicada
        // descreveria a tabela ANTERIOR, e uma captura disparada nesses frames sairia afirmando um
        // fluxo total que nao era o da tabela em uso.
        DistStats       = FDistributionStats{};

        // Estatico: sem isto a extracao rodaria todo frame reconstruindo o mesmo dado.
        Dirty = false;
    }
}
