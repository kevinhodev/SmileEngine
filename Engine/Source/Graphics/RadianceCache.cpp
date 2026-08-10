#include "Smile/Graphics/RadianceCache.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/FrameContext.h"
#include "Smile/Graphics/SceneTargets.h"
#include "Smile/Graphics/DebugTargets.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        // Espelham o layout do RadianceCacheResolve.cs.hlsl. Entries e 1 uint (checksum), Accum
        // sao 4 uints (rgb em ponto fixo + contagem) e Resolved e 1 uint4.
        constexpr u32 kEntryStride    = sizeof(u32);
        constexpr u32 kAccumStride    = sizeof(u32) * 4;
        constexpr u32 kResolvedStride = sizeof(u32) * 4;
        // Espelha RC_STAT_COUNT do RadianceCache.hlsli.
        constexpr u32 kStatCount      = 6;
        constexpr u32 kStatBytes      = kStatCount * sizeof(u32);
        // Modos do dispatch do resolve (StatsParams.x). Ver o cabecalho do shader.
        constexpr f32 kModeResolve    = 0.0f;
        constexpr f32 kModeClearStats = 1.0f;
        constexpr DXGI_FORMAT kDebugFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        ComPtr<ID3D12Resource> CreateStructuredUAV(ID3D12Device* Device, u32 Count, u32 Stride,
                                                   const char* Label) {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = static_cast<UINT64>(Count) * Stride;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> Buffer;
            SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Buffer)));
            VramTracker::Register(Buffer.Get(), EVramCategory::GI, Label);
            return Buffer;
        }
    }

    void FRadianceCache::Initialize(ID3D12Device* Device) {
        // 1 SRV / 4 UAVs. O shader nao le SRV nenhum — o caminho de dados do cache e todo UAV,
        // porque o ShadeSurfaceHit le e escreve na mesma invocacao. Mas um range de 0 descritores
        // nao serializa em D3D12, entao o root param 1 existe e o RecordResolve o aponta para o
        // BindingSRV. Os 4 UAVs precisam ser CONTIGUOS — sao um range so.
        ResolvePSO.Initialize(Device, "RadianceCacheResolve.cs_6_6.cso", 1, 4, false);
        // Visualizador: 2 SRVs (depth, normal) + 1 UAV de saida. Bindless para chegar na tabela
        // do cache, igual aos traces.
        DebugPSO.Initialize(Device, "RadianceCacheDebug.cs_6_6.cso", 2, 1, true);
        CreateConstantBuffer(Device);
        Initialized = true;
    }

    void FRadianceCache::OnRecreatePipelines(const FPassInitContext& Ctx) {
        if (!Initialized || !Ctx.Device) return;
        ResolvePSO.Initialize(Ctx.Device, "RadianceCacheResolve.cs_6_6.cso", 1, 4, false);
        DebugPSO.Initialize(Ctx.Device, "RadianceCacheDebug.cs_6_6.cso", 2, 1, true);
        // Full HLSL reloads may change the shared hash/key semantics. Old entries are then
        // undecodable by the new query code even though the buffers themselves survived.
        if (Ready) ResetOnce();
    }

    FPassShaderStems FRadianceCache::ShaderStems() const {
        // O RadianceCache.hlsli nao entra: header nao gera .cso. Editar um .hlsli dispara o
        // reload COMPLETO, que passa por aqui de qualquer forma — e tambem pelos traces que o
        // incluem, que e o que importa quando o hash muda.
        static const char* const Stems[] = { "RadianceCacheResolve", "RadianceCacheDebug" };
        return { Stems, 2u };
    }

    bool FRadianceCache::IsActive(const FFrameModes&) const {
        // Nao depende de qual consumidor roda: o resolve tem de rodar sempre que houve escrita,
        // e a escrita vem de dentro do ShadeSurfaceHit, que os tres traces compartilham. Gatear
        // por ReSTIRGIActive deixaria o Accum sujo do frame anterior entrar na media seguinte.
        return IsInitialized() && Enabled;
    }

    void FRadianceCache::CreateConstantBuffer(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * 2ull *
                                sizeof(RadianceCacheConstants);
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&CB)));
        D3D12_RANGE NoRead{ 0, 0 };
        SMILE_HR(CB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedCB)));

        Desc.Width = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                     sizeof(RadianceCacheDebugConstants);
        SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&DebugCB)));
        SMILE_HR(DebugCB->Map(0, &NoRead, reinterpret_cast<void**>(&MappedDebugCB)));
    }

    D3D12_GPU_VIRTUAL_ADDRESS FRadianceCache::CBAddr(u32 Variant) const {
        const UINT64 Index = static_cast<UINT64>(FrameSlot) * 2ull + Variant;
        return CB->GetGPUVirtualAddress() + Index * sizeof(RadianceCacheConstants);
    }

    D3D12_GPU_VIRTUAL_ADDRESS FRadianceCache::DebugCBAddr() const {
        return DebugCB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(RadianceCacheDebugConstants);
    }

    u64 FRadianceCache::MemoryBytes() const {
        if (!Ready) return 0;
        return static_cast<u64>(CapacityV) * (kEntryStride + kAccumStride + kResolvedStride);
    }

    void FRadianceCache::Release(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(UavTable, 4);
        Free(BindingSRV, 1);
        Entries.Reset(); Accum.Reset(); Resolved.Reset(); StatsBuf.Reset();
        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
            StatsReadback[f].Reset();
            StatsReadbackPending[f] = false;
        }
        StatsCPU = {};
        EntriesState = AccumState = ResolvedState = D3D12_RESOURCE_STATE_COMMON;
        StatsState   = D3D12_RESOURCE_STATE_COMMON;
        CapacityV = 0;
        Ready = false;
    }

    void FRadianceCache::ReleaseDebug(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        Free(DebugSRV, 1);
        Free(DebugUAV, 1);
        Free(DebugSrvTable, 2);
        DebugTex.Reset();
        DebugTexState = D3D12_RESOURCE_STATE_COMMON;
        DebugWidth = DebugHeight = 0;
    }

    void FRadianceCache::SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                                       u32 CapacityLog2) {
        if (!Initialized) return;
        Release(SRVHeap);

        CapacityLog2 = std::clamp(CapacityLog2, 16u, kMaxCapacityLog2);
        CapacityV    = 1u << CapacityLog2;

        Entries  = CreateStructuredUAV(Device, CapacityV, kEntryStride,
                                       "Radiance cache · chaves");
        Accum    = CreateStructuredUAV(Device, CapacityV * 4u, kEntryStride,
                                       "Radiance cache · acumulacao");
        Resolved = CreateStructuredUAV(Device, CapacityV, kResolvedStride,
                                       "Radiance cache · resolvido");
        StatsBuf = CreateStructuredUAV(Device, kStatCount, kEntryStride,
                                       "Radiance cache · contadores");

        // Anel de readback dos contadores (ver o membro StatsReadback).
        {
            D3D12_HEAP_PROPERTIES RbHeap{};
            RbHeap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC RbDesc{};
            RbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            RbDesc.Width            = kStatBytes;
            RbDesc.Height           = 1;
            RbDesc.DepthOrArraySize = 1;
            RbDesc.MipLevels        = 1;
            RbDesc.Format           = DXGI_FORMAT_UNKNOWN;
            RbDesc.SampleDesc       = { 1, 0 };
            RbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                SMILE_HR(Device->CreateCommittedResource(&RbHeap, D3D12_HEAP_FLAG_NONE, &RbDesc,
                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                         IID_PPV_ARGS(&StatsReadback[f])));
                StatsReadbackPending[f] = false;
            }
        }

        // Os 4 UAVs em slots contiguos: o root param 2 do ResolvePSO e UM range de 4.
        UavTable = SRVHeap.Allocate(4);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format                     = DXGI_FORMAT_UNKNOWN;
        Uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.NumElements         = CapacityV;
        Uav.Buffer.StructureByteStride = kEntryStride;
        SRVHeap.CreateUAV(Device, Entries.Get(), Uav, UavTable + 0);

        // O Accum e declarado como RWStructuredBuffer<uint> no shader e indexado por
        // entryIndex*4 + canal — por isso NumElements e 4x e o stride e de uint, nao do quarteto.
        Uav.Buffer.NumElements         = CapacityV * 4u;
        Uav.Buffer.StructureByteStride = kEntryStride;
        SRVHeap.CreateUAV(Device, Accum.Get(), Uav, UavTable + 1);

        Uav.Buffer.NumElements         = CapacityV;
        Uav.Buffer.StructureByteStride = kResolvedStride;
        SRVHeap.CreateUAV(Device, Resolved.Get(), Uav, UavTable + 2);

        Uav.Buffer.NumElements         = kStatCount;
        Uav.Buffer.StructureByteStride = kEntryStride;
        SRVHeap.CreateUAV(Device, StatsBuf.Get(), Uav, UavTable + 3);

        // Descritor de amarracao para o root param 1 do ResolvePSO (ver o membro BindingSRV).
        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.Format                     = DXGI_FORMAT_UNKNOWN;
        Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Buffer.NumElements         = CapacityV;
        Srv.Buffer.StructureByteStride = kEntryStride;
        BindingSRV = SRVHeap.Allocate(1);
        SRVHeap.CreateSRV(Device, Entries.Get(), Srv, BindingSRV);

        // Os buffers nascem com lixo (CreateCommittedResource nao zera). Uma chave aleatoria que
        // por acaso valha um checksum plausivel devolveria radiancia inventada no primeiro frame,
        // entao o primeiro resolve TEM que ser um reset.
        ResetPending = true;
        Ready = true;

        LogDebug("[GI] - Radiance cache: " + std::to_string(CapacityV) + " celulas, " +
                 std::to_string(MemoryBytes() / (1024ull * 1024ull)) + " MB, celula base " +
                 std::to_string(BaseCellSize) + " m");
    }

    void FRadianceCache::UpdatePerFrame(u32 InFrameSlot, const Vec3& InCameraPos) {
        FrameSlot = InFrameSlot;
        CameraPos = InCameraPos;
        if (!Ready || !MappedCB) return;
        CPU.CacheParams = { static_cast<f32>(CapacityV),
                            static_cast<f32>(kMaxAccumSamples),
                            static_cast<f32>(kStaleFrameMax),
                            ResetPending ? 1.0f : 0.0f };
        const size_t Base = static_cast<size_t>(FrameSlot) * 2u * sizeof(RadianceCacheConstants);
        CPU.StatsParams = { kModeResolve, 0.0f, 0.0f, 0.0f };
        std::memcpy(MappedCB + Base, &CPU, sizeof(CPU));
        CPU.StatsParams = { kModeClearStats, 0.0f, 0.0f, 0.0f };
        std::memcpy(MappedCB + Base + sizeof(RadianceCacheConstants), &CPU, sizeof(CPU));
    }

    void FRadianceCache::Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                                    D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After) {
        if (State == After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = Resource;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = State;
        B.Transition.StateAfter  = After;
        CL->ResourceBarrier(1, &B);
        State = After;
    }

    void FRadianceCache::TransitionForTrace(ID3D12GraphicsCommandList* CL) {
        if (!Ready) return;
        // Os tres ficam em UNORDERED_ACCESS o frame inteiro. O ShadeSurfaceHit LE Entries e
        // Resolved e ESCREVE Entries e Accum na mesma invocacao: se a leitura fosse por SRV o
        // recurso teria que estar em NON_PIXEL e em UAV ao mesmo tempo. Ver o cabecalho do
        // RadianceCache.hlsli.
        Transition(CL, Entries.Get(),  EntriesState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Accum.Get(),    AccumState,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, StatsBuf.Get(), StatsState,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void FRadianceCache::RecordResolve(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!Ready) return;
        const bool ResetThisFrame = ResetPending;

        Transition(CL, Entries.Get(),  EntriesState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Accum.Get(),    AccumState,    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, Resolved.Get(), ResolvedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Os tres JA estao em UNORDERED_ACCESS desde o TransitionForTrace, entao as chamadas
        // acima sao no-op — e e exatamente por isso que a barreira de UAV precisa ser explicita.
        // Sem ela nada ordena as escritas atomicas dos traces contra a leitura do resolve, e o
        // sintoma seria a media incorporar um Accum parcial: celulas escuras intermitentes, sem
        // padrao, que passariam facil por ruido de amostragem.
        D3D12_RESOURCE_BARRIER Uav[3]{};
        for (u32 i = 0; i < 3; ++i) Uav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Uav[0].UAV.pResource = Entries.Get();
        Uav[1].UAV.pResource = Accum.Get();
        Uav[2].UAV.pResource = Resolved.Get();
        CL->ResourceBarrier(3, Uav);

        // COPIA ANTES DE ZERAR. Os contadores de QUERY foram escritos pelos traces, que ja
        // rodaram neste frame; os de OCUPACAO, pelo resolve, que ainda vai rodar. Copiar aqui
        // captura query deste frame e ocupacao do frame ANTERIOR. Um frame de defasagem num
        // painel de diagnostico e invisivel; a alternativa (zerar antes) apagaria a medida de
        // query do proprio frame, que e justamente a mais cara de obter.
        // No primeiro resolve os buffers ainda contem o lixo de CreateCommittedResource.
        // Copiar os contadores antes do reset publicaria uma amostra espuria no painel.
        if (!ResetThisFrame) {
            Transition(CL, StatsBuf.Get(), StatsState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            CL->CopyBufferRegion(StatsReadback[FrameSlot].Get(), 0, StatsBuf.Get(), 0, kStatBytes);
            StatsReadbackPending[FrameSlot] = true;
            Transition(CL, StatsBuf.Get(), StatsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        ResolvePSO.Bind(CL);
        // Root param 1 existe so para o range de 1 SRV do root signature nao ficar sem binding
        // (ver Initialize); o shader nao le nada dele.
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(BindingSRV));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(UavTable));

        // Zera os contadores num dispatch proprio: nada ordena "zerar" contra "acumular" dentro
        // de um mesmo dispatch, entre grupos.
        CL->SetComputeRootConstantBufferView(0, CBAddr(1));
        CL->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER StatsBarrier{};
        StatsBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        StatsBarrier.UAV.pResource = StatsBuf.Get();
        CL->ResourceBarrier(1, &StatsBarrier);

        CL->SetComputeRootConstantBufferView(0, CBAddr(0));
        CL->Dispatch((CapacityV + 63u) / 64u, 1, 1);

        ResetPending = false;
    }

    void FRadianceCache::OnResize(const FPassInitContext& Ctx) {
        if (!Initialized || !Ctx.Device || !Ctx.SRVHeap || !Ctx.Targets) return;
        FTextureSRVHeap& SRVHeap = *Ctx.SRVHeap;
        ReleaseDebug(SRVHeap);
        if (Ctx.RenderWidth == 0 || Ctx.RenderHeight == 0) return;

        const u32 DepthSlot  = Ctx.Targets->DepthSRVSlot;
        const u32 NormalSlot = Ctx.Targets->NormalSRVSlot;
        // O NormalBuffer e opcional na engine (nasceu com a GTAO). Sem ele o visualizador nao tem
        // como reproduzir a chave — que usa a normal geometrica — entao ele simplesmente nao
        // existe, em vez de mentir com a normal de sombreamento.
        if (DepthSlot == FSceneTargets::kInvalidSlot ||
            NormalSlot == FSceneTargets::kInvalidSlot) {
            return;
        }

        DebugWidth  = Ctx.RenderWidth;
        DebugHeight = Ctx.RenderHeight;

        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width            = DebugWidth;
        Desc.Height           = DebugHeight;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = kDebugFormat;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        SMILE_HR(Ctx.Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&DebugTex)));
        VramTracker::Register(DebugTex.Get(), EVramCategory::GI, "Radiance cache · visualizador");
        DebugTexState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
        Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Srv.Format                  = kDebugFormat;
        Srv.Texture2D.MipLevels     = 1;
        DebugSRV = SRVHeap.Allocate(1);
        SRVHeap.CreateSRV(Ctx.Device, DebugTex.Get(), Srv, DebugSRV);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Uav.Format        = kDebugFormat;
        DebugUAV = SRVHeap.Allocate(1);
        SRVHeap.CreateUAV(Ctx.Device, DebugTex.Get(), Uav, DebugUAV);

        // t0/t1 contiguos: o root param 1 do DebugPSO e UM range de 2.
        DebugSrvTable = SRVHeap.Allocate(2);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(DebugSrvTable);
            D3D12_CPU_DESCRIPTOR_HANDLE Src[2] = {
                SRVHeap.CpuHandleStaging(DepthSlot),
                SRVHeap.CpuHandleStaging(NormalSlot),
            };
            UINT Two = 2; UINT Ones[2] = { 1, 1 };
            Ctx.Device->CopyDescriptors(1, &Dst, &Two, 2, Src, Ones,
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

    }

    void FRadianceCache::OnRegisterDebugTargets() {
        // O OnResize acima pode ter desistido (sem NormalBuffer, ou render-res zerada) e deixado
        // o alvo inexistente. Publicar mesmo assim poria um SRV invalido na UI.
        if (!DebugTex || DebugSRV == kInvalidSlot) return;

        // Raw: o shader ja escreve cor em faixa visivel (falsa-cor de diagnostico, nao radiancia).
        // Passar por HDR/tonemap distorceria a leitura — verde e vermelho tem que sair como sao.
        DebugTargets::Register(kDebugTargetName, DebugSRV, EDebugDecode::Raw,
                               0, 1, 1.0f, 0, /*LinearFilter*/ false);
    }

    void FRadianceCache::RecordDebug(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                                     const Mat44& InvViewProj, const Vec3& InCameraPos,
                                     u32 InFrameSlot) {
        if (!Ready || !DebugTex || DebugSrvTable == kInvalidSlot || !MappedDebugCB) return;

        // Os traces do frame podem ter acabado de publicar novas chaves. Entries continua em UAV,
        // portanto uma transition seria no-op; a barreira UAV e o que torna essas escritas
        // visiveis para a varredura do visualizador.
        D3D12_RESOURCE_BARRIER CacheBarrier{};
        CacheBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        CacheBarrier.UAV.pResource = Entries.Get();
        CL->ResourceBarrier(1, &CacheBarrier);

        RadianceCacheDebugConstants D{};
        D.InvViewProj   = InvViewProj;
        D.CameraPosMode = { InCameraPos.X, InCameraPos.Y, InCameraPos.Z,
                            static_cast<f32>(DebugMode) };
        D.ScreenParams  = { static_cast<f32>(DebugWidth), static_cast<f32>(DebugHeight),
                            1.0f / static_cast<f32>(DebugWidth),
                            1.0f / static_cast<f32>(DebugHeight) };
        // O visualizador consulta a tabela mas nunca escreve nela: params SEM update e SEM stats.
        // Um dispatch de tela cheia contando "queries" poluiria a taxa de acerto que o painel
        // mostra, que e sobre os RAIOS e nao sobre os pixels.
        const FRadianceCacheShaderParams P = ShaderParams(/*AllowUpdate*/ false);
        D.RadianceCacheCamCell     = P.CameraPosCell;
        D.RadianceCacheLodCapFlags = P.LodCapacityFlags;
        D.RadianceCacheResources   = P.Resources;
        std::memcpy(MappedDebugCB +
                        static_cast<size_t>(InFrameSlot) * sizeof(RadianceCacheDebugConstants),
                    &D, sizeof(D));

        Transition(CL, DebugTex.Get(), DebugTexState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        DebugPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, DebugCBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(DebugSrvTable));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(DebugUAV));
        CL->Dispatch((DebugWidth + 7u) / 8u, (DebugHeight + 7u) / 8u, 1);
        Transition(CL, DebugTex.Get(), DebugTexState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void FRadianceCache::CollectStats(u32 InFrameSlot) {
        if (InFrameSlot >= FCommandQueue::kFramesInFlight ||
            !StatsReadbackPending[InFrameSlot] || !StatsReadback[InFrameSlot]) {
            return;
        }
        StatsReadbackPending[InFrameSlot] = false;

        void* Mapped = nullptr;
        D3D12_RANGE ReadRange{ 0, kStatBytes };
        if (FAILED(StatsReadback[InFrameSlot]->Map(0, &ReadRange, &Mapped)) || !Mapped) return;
        const u32* S = static_cast<const u32*>(Mapped);
        StatsCPU.Occupied = S[0];
        StatsCPU.Valid    = S[1];
        StatsCPU.Samples  = S[2];
        StatsCPU.Evicted  = S[3];
        StatsCPU.Queries  = S[4];
        StatsCPU.Hits     = S[5];
        D3D12_RANGE NoWrite{ 0, 0 };
        StatsReadback[InFrameSlot]->Unmap(0, &NoWrite);
    }

    FRadianceCacheShaderParams FRadianceCache::ShaderParams(bool AllowUpdate) const {
        FRadianceCacheShaderParams P{};
        // Sem cena montada o shader nao pode nem consultar nem escrever: os indices bindless
        // apontariam para descritores livres.
        // Reset acontece no resolve, no fim do frame. Ate ele terminar, consultar devolveria uma
        // tabela do regime anterior (e atualizar seria trabalho descartado alguns passes depois).
        // O resolve continua ativo via IsActive; so os consumidores recebem flags zeradas.
        const bool On = Enabled && Ready && !ResetPending;
        u32 Flags = 0u;
        if (On) {
            if (QueryEnabled) Flags |= kFlagQuery;
            if (AllowUpdate)  Flags |= kFlagUpdate;
            // So conta acerto/erro quando ha o que contar: sem query o contador so mediria zero
            // e ainda assim pagaria os atomicos.
            if (StatsEnabled && QueryEnabled) Flags |= kFlagStats;
        }
        P.CameraPosCell    = { CameraPos.X, CameraPos.Y, CameraPos.Z, BaseCellSize };
        P.LodCapacityFlags = { LodDistance, static_cast<f32>(CapacityV),
                               static_cast<f32>(Flags), 0.0f };
        P.Resources        = { static_cast<f32>(UavTable + 0),
                               static_cast<f32>(UavTable + 1),
                               static_cast<f32>(UavTable + 2),
                               static_cast<f32>(UavTable + 3) };
        return P;
    }
}
