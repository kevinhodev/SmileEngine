#include "Smile/Graphics/GI/DDGI.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/RayTracing/RTMasks.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kAtlasFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* _Device, u32 _W, u32 _H,
                                           DXGI_FORMAT _Fmt, const char* _Label = nullptr) {
            return GpuResources::CreateTex2D(_Device, _W, _H, _Fmt,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_COMMON, EVramCategory::GI,
                                             nullptr, 1, 1, _Label);
        }

        ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* _Device, UINT64 _Size,
                                                   D3D12_RESOURCE_STATES _State,
                                                   D3D12_RESOURCE_FLAGS _Flags = D3D12_RESOURCE_FLAG_NONE) {
            return GpuResources::CreateBuffer(_Device, _Size, _Flags, _State, EVramCategory::GI,
                                              "DDGI · buffers");
        }

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* _Device, UINT64 _Size,
                                                  u8** _MappedOut) {
            GpuResources::FUploadBuffer Upload =
                GpuResources::CreateUploadBuffer(_Device, _Size, 1, /*ForConstantBuffer*/ false);
            if (_MappedOut) *_MappedOut = Upload.Mapped;
            return Upload.Resource;
        }
    }

    void FDDGI::Initialize(ID3D12Device* _Device) {
        CreatePipelines(_Device);
        CreateConstantBuffer(_Device);
        Initialized = true;
    }

    void FDDGI::CreateConstantBuffer(ID3D12Device* _Device) {
        const UINT64 Size = static_cast<UINT64>(FCommandQueue::kFramesInFlight) * sizeof(DDGIConstants);
        CB = CreateUploadBuffer(_Device, Size, &MappedCB);
    }

    void FDDGI::ReleaseSceneResources(FTextureSRVHeap& _SRVHeap) {
        auto FreeSlot = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { _SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        FreeSlot(AtlasSRVSlot, 1);
        FreeSlot(AtlasUAVSlot, 1);
        FreeSlot(DistSRVSlot, 1);
        FreeSlot(DistUAVSlot, 1);
        FreeSlot(ProbesTraceSRVSlot, 1);
        FreeSlot(ProbesTraceUAVSlot, 1);
        FreeSlot(ProbeDataSRVSlot, 1);
        FreeSlot(ProbeRayCountSRVSlot, 1);
        FreeSlot(ProbeDataUAVSlot, 2); 
        ProbeRayCountUAVSlot = kInvalidSlot;
        FreeSlot(ActiveProbeIndicesSRVSlot, 1);
        FreeSlot(ActiveProbeCountSRVSlot, 1);
        FreeSlot(ActiveProbeBuildUAVStart, 2);
        ActiveProbeIndicesUAVSlot = kInvalidSlot;
        ActiveProbeCountUAVSlot   = kInvalidSlot;
        FreeSlot(ActiveProbeDispatchArgsUAVSlot, 1);
        for (u32 i = 0; i < kTraceTables; ++i) FreeSlot(TraceTable[i], 11);
        FreeSlot(SceneGITableStart_, 3);
        FreeSlot(UpdateTableStart, 4);
        IrradAtlas.Reset();
        DistAtlas.Reset();
        ProbesTrace.Reset();
        ProbeDataBuf.Reset();
        ProbeRayCountBuf.Reset();
        ActiveProbeIndicesBuf.Reset();
        ActiveProbeCountBuf.Reset();
        ActiveProbeDispatchArgsBuf.Reset();
        if (ActiveProbeCountReadback && MappedActiveProbeCount) ActiveProbeCountReadback->Unmap(0, nullptr);
        ActiveProbeCountReadback.Reset();
        MappedActiveProbeCount = nullptr;
        AtlasState         = D3D12_RESOURCE_STATE_COMMON;
        DistState          = D3D12_RESOURCE_STATE_COMMON;
        ProbesState        = D3D12_RESOURCE_STATE_COMMON;
        ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;
        ActiveProbeIndicesState = D3D12_RESOURCE_STATE_COMMON;
        ActiveProbeCountState   = D3D12_RESOURCE_STATE_COMMON;
        ActiveProbeDispatchArgsState = D3D12_RESOURCE_STATE_COMMON;
        LastUpdateUsedProbeCompaction_ = false;
        LastActiveProbeCount_ = 0;
        LastCompactedProbeCapacity_ = 0;
        for (u32 I = 0; I < kTraceTables; ++I) {
            CompactionReadbackIssued_[I] = false;
            CompactionReadbackCapacity_[I] = 0;
        }
        Ready = false;
    }

    void FDDGI::InvalidateRegion(const Vec3& _Min, const Vec3& _Max, EGIRegionChange _Change) {
        // Irradiancia e distancia compartilham uma caixa unificada de forma conservadora.
        if (InvalidateFramesLeft_ > 0 || InvalidateDistFramesLeft_ > 0) {
            InvalidateMin_ = { std::min(InvalidateMin_.X, _Min.X),
                               std::min(InvalidateMin_.Y, _Min.Y),
                               std::min(InvalidateMin_.Z, _Min.Z) };
            InvalidateMax_ = { std::max(InvalidateMax_.X, _Max.X),
                               std::max(InvalidateMax_.Y, _Max.Y),
                               std::max(InvalidateMax_.Z, _Max.Z) };
        } else {
            InvalidateMin_ = _Min;
            InvalidateMax_ = _Max;
        }
        // Guarda a caixa crua; UpdatePerFrame aplica a folga uma vez apos a uniao.
        InvalidateFramesLeft_     = kInvalidateFrames;
        InvalidateDistFramesLeft_ = kInvalidateDistFrames;
        if (_Change == EGIRegionChange::Geometry) ReclassifyPending_ = true;
    }

    void FDDGI::SetupForScene(ID3D12Device* _Device, FCommandQueue& _Queue,
                              FTextureSRVHeap& _SRVHeap, const FScene& _Scene,
                              const Vec3& _AABBMin, const Vec3& _AABBMax,
                              u32 _TlasSRVSlot, u32 _SkyViewSRVSlot, u32 _InstanceGeoSRVSlot) {
        if (!Initialized) return;
        ReleaseSceneResources(_SRVHeap);

        const u32 NumRenderables = static_cast<u32>(_Scene.Renderables().size());
        if (NumRenderables == 0 || _TlasSRVSlot == kInvalidSlot) {
            LogDebug("[GI] - DDGI: Cena sem Geometria/TLAS; Volume nao Criado");
            return;
        }

        Vec3 ext = { std::max(_AABBMax.X - _AABBMin.X, 0.1f),
                     std::max(_AABBMax.Y - _AABBMin.Y, 0.1f),
                     std::max(_AABBMax.Z - _AABBMin.Z, 0.1f) };
        f32 maxExt = std::max(ext.X, std::max(ext.Y, ext.Z));
        // Relaxa a densidade em runtime ate atlas, textura de trace e dispatch caberem.
        constexpr int kTargetMax  = 24;
        constexpr int kMaxPerAxis = 128;
        constexpr u64 kTexMax     = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        CascadeCount_ = DesiredCascades < 1 ? 1u
                      : (DesiredCascades > kMaxCascades ? kMaxCascades : DesiredCascades);

        Cascades[0].Spacing = std::max(maxExt / (kTargetMax - 1), 0.5f);
        auto axisCount = [&](f32 e) {
            int n = static_cast<int>(std::ceil(e / Cascades[0].Spacing)) + 1;
            return std::clamp(n, 2, kMaxPerAxis);
        };
        // As bandas contêm fileiras X completas para preservar a localidade 2x2 do gather.
        auto atlasGridFor = [](u32 CX, u32 CY, u32 CZ, u32 Cascades_, u32& OutPerRow,
                               u32& OutRows) {
            CX = std::max(CX, 1u); CY = std::max(CY, 1u); CZ = std::max(CZ, 1u);
            Cascades_ = std::max(Cascades_, 1u);
            OutPerRow = CX; OutRows = CY * CZ;
            u64 Best  = 0;
            for (u32 ZRows = 1; ZRows <= CZ; ++ZRows) {
                const u32 PerRow = CX * ZRows;
                const u32 Rows   = ((CZ + ZRows - 1) / ZRows) * CY;
                const u64 Score  = std::max<u64>(PerRow, static_cast<u64>(Rows) * Cascades_);
                if (Best == 0 || Score < Best) { Best = Score; OutPerRow = PerRow; OutRows = Rows; }
            }
        };
        auto gridFits = [&] {
            const u64 PerCascade = static_cast<u64>(CountX) * CountY * CountZ;
            const u64 Probes     = PerCascade * CascadeCount_;
            u32 PerRow32 = 1, RowsPerCascade32 = 1;
            atlasGridFor(static_cast<u32>(CountX), static_cast<u32>(CountY),
                         static_cast<u32>(CountZ), CascadeCount_, PerRow32, RowsPerCascade32);
            const u64 PerRow = PerRow32;
            const u64 Rows   = static_cast<u64>(RowsPerCascade32) * CascadeCount_;
            const u64 TraceRow = std::min<u64>(Probes, kTraceProbesPerRow);
            constexpr u64 kMaxGroups = 65535;
            return PerRow * (kDistTileSize + 2) <= kTexMax &&
                   Rows   * (kDistTileSize + 2) <= kTexMax &&
                   TraceRow * kRaysPerProbe <= kTexMax &&
                   (Probes + TraceRow - 1) / TraceRow <= kTexMax &&
                   DispatchGroupsX(static_cast<u32>(Probes)) <= kMaxGroups &&
                   DispatchGroupsY(static_cast<u32>(Probes)) <= kMaxGroups;
        };
        CountX = axisCount(ext.X); CountY = axisCount(ext.Y); CountZ = axisCount(ext.Z);
        if (!gridFits()) {
            const f32 Requested = Cascades[0].Spacing;
            while (!gridFits()) {
                Cascades[0].Spacing *= 1.05f;
                CountX = axisCount(ext.X); CountY = axisCount(ext.Y); CountZ = axisCount(ext.Z);
            }
            LogWarning("[GI] - DDGI: espacamento de " + std::to_string(Requested) +
                       " m nao cabe nos atlas (limite de dimensao de textura); aberto para " +
                       std::to_string(Cascades[0].Spacing) + " m");
        }
        // Os recursos sao dimensionados pela cascata grossa que cobre a cena.
        const f32 CoarseSpacing = Cascades[0].Spacing;
        const Vec3 CoarseMin{ _AABBMin.X - 0.5f * CoarseSpacing,
                              _AABBMin.Y - 0.5f * CoarseSpacing,
                              _AABBMin.Z - 0.5f * CoarseSpacing };
        for (u32 C = 0; C < kMaxCascades; ++C) {
            Cascades[C] = FCascade{};
            Cascades[C].GridMin = CoarseMin;
            Cascades[C].Spacing = CoarseSpacing;
        }

        ProbesPerCascade_ = static_cast<u32>(CountX) * CountY * CountZ;
        NumProbes         = ProbesPerCascade_ * CascadeCount_;
        ScheduledProbeCount_     = NumProbes;
        ScheduledCascadeCount_   = CascadeCount_;
        LastUpdatedCascadeCount_ = 0;
        UpdateSerial_            = 0;
        LastForcedUpdateSerial_  = 0;
        CoarseDue_               = true;
        ScheduledFullForced_     = false;
        for (u32 C = 0; C < kMaxCascades; ++C) CascadeUpdateAge_[C] = 0;

        // O shader deriva TilesPerRow da largura; ambos os atlas usam a mesma grade.
        atlasGridFor(static_cast<u32>(CountX), static_cast<u32>(CountY),
                     static_cast<u32>(CountZ), CascadeCount_, TilesPerRow, TileRowsPerCascade);
        const u32 TileRows = TileRowsPerCascade * CascadeCount_;
        AtlasWidth      = TilesPerRow * (kTileSize + 2);
        AtlasHeight     = TileRows    * (kTileSize + 2);
        DistAtlasWidth  = TilesPerRow * (kDistTileSize + 2);
        DistAtlasHeight = TileRows    * (kDistTileSize + 2);
        const u32 TraceProbesPerRow = std::min<u32>(NumProbes, kTraceProbesPerRow);
        const u32 TraceRows         = (NumProbes + TraceProbesPerRow - 1) / TraceProbesPerRow;
        MaxRayDist      = std::sqrt(ext.X * ext.X + ext.Y * ext.Y + ext.Z * ext.Z) * 1.5f;

        IrradAtlas  = CreateTex2D(_Device, AtlasWidth, AtlasHeight, kAtlasFormat,
                                  "DDGI · atlas irradiancia");
        DistAtlas   = CreateTex2D(_Device, DistAtlasWidth, DistAtlasHeight, DXGI_FORMAT_R16G16_FLOAT,
                                  "DDGI · atlas distancia");
        ProbesTrace = CreateTex2D(_Device, TraceProbesPerRow * kRaysPerProbe, TraceRows,
                                  kAtlasFormat, "DDGI · raios por sonda");

        ProbeDataBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(Vec4),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ProbeRayCountBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(u32),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ActiveProbeIndicesBuf = CreateDefaultBuffer(
            _Device, static_cast<UINT64>(NumProbes) * sizeof(u32), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ActiveProbeCountBuf = CreateDefaultBuffer(
            _Device, sizeof(u32), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ActiveProbeDispatchArgsBuf = CreateDefaultBuffer(
            _Device, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ActiveProbeCountReadback = GpuResources::CreateReadbackBuffer(
            _Device, static_cast<u64>(FCommandQueue::kFramesInFlight) * sizeof(u32));
        void* ActiveReadback = nullptr;
        SMILE_HR(ActiveProbeCountReadback->Map(0, nullptr, &ActiveReadback));
        MappedActiveProbeCount = reinterpret_cast<u8*>(ActiveReadback);

        AtlasSRVSlot       = _SRVHeap.Allocate(1);
        AtlasUAVSlot       = _SRVHeap.Allocate(1);
        DistSRVSlot        = _SRVHeap.Allocate(1);
        DistUAVSlot        = _SRVHeap.Allocate(1);
        ProbesTraceSRVSlot = _SRVHeap.Allocate(1);
        ProbesTraceUAVSlot = _SRVHeap.Allocate(1);
        ProbeDataSRVSlot     = _SRVHeap.Allocate(1);
        ProbeRayCountSRVSlot = _SRVHeap.Allocate(1);
        ActiveProbeIndicesSRVSlot = _SRVHeap.Allocate(1);
        ActiveProbeCountSRVSlot   = _SRVHeap.Allocate(1);

        const u32 RelocUAVBase = _SRVHeap.Allocate(2);
        ProbeDataUAVSlot     = RelocUAVBase;
        ProbeRayCountUAVSlot = RelocUAVBase + 1;
        ActiveProbeBuildUAVStart  = _SRVHeap.Allocate(2);
        ActiveProbeIndicesUAVSlot = ActiveProbeBuildUAVStart;
        ActiveProbeCountUAVSlot   = ActiveProbeBuildUAVStart + 1;
        ActiveProbeDispatchArgsUAVSlot = _SRVHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC Tex2DSrv{};
        Tex2DSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        Tex2DSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Tex2DSrv.Format                  = kAtlasFormat;
        Tex2DSrv.Texture2D.MipLevels     = 1;
        _SRVHeap.CreateSRV(_Device, IrradAtlas.Get(),  Tex2DSrv, AtlasSRVSlot);
        _SRVHeap.CreateSRV(_Device, ProbesTrace.Get(), Tex2DSrv, ProbesTraceSRVSlot);
        Tex2DSrv.Format = DXGI_FORMAT_R16G16_FLOAT;
        _SRVHeap.CreateSRV(_Device, DistAtlas.Get(), Tex2DSrv, DistSRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC Tex2DUav{};
        Tex2DUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Tex2DUav.Format        = kAtlasFormat;
        _SRVHeap.CreateUAV(_Device, IrradAtlas.Get(),  Tex2DUav, AtlasUAVSlot);
        _SRVHeap.CreateUAV(_Device, ProbesTrace.Get(), Tex2DUav, ProbesTraceUAVSlot);
        Tex2DUav.Format = DXGI_FORMAT_R16G16_FLOAT;
        _SRVHeap.CreateUAV(_Device, DistAtlas.Get(), Tex2DUav, DistUAVSlot);

        D3D12_SHADER_RESOURCE_VIEW_DESC BufSrv{};
        BufSrv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        BufSrv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        BufSrv.Buffer.FirstElement        = 0;
        BufSrv.Format                     = DXGI_FORMAT_R32G32B32A32_FLOAT;
        BufSrv.Buffer.NumElements         = NumProbes;
        BufSrv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeDataBuf.Get(), BufSrv, ProbeDataSRVSlot);

        BufSrv.Format                     = DXGI_FORMAT_R32_UINT;
        BufSrv.Buffer.NumElements         = NumProbes;
        BufSrv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeRayCountBuf.Get(), BufSrv, ProbeRayCountSRVSlot);
        _SRVHeap.CreateSRV(_Device, ActiveProbeIndicesBuf.Get(), BufSrv, ActiveProbeIndicesSRVSlot);
        BufSrv.Buffer.NumElements = 1;
        _SRVHeap.CreateSRV(_Device, ActiveProbeCountBuf.Get(), BufSrv, ActiveProbeCountSRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC BufUav{};
        BufUav.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        BufUav.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;
        BufUav.Buffer.FirstElement = 0;
        BufUav.Buffer.NumElements  = NumProbes;
        _SRVHeap.CreateUAV(_Device, ProbeDataBuf.Get(), BufUav, ProbeDataUAVSlot);
        BufUav.Format              = DXGI_FORMAT_R32_UINT;
        _SRVHeap.CreateUAV(_Device, ProbeRayCountBuf.Get(), BufUav, ProbeRayCountUAVSlot);
        BufUav.Buffer.NumElements = NumProbes;
        _SRVHeap.CreateUAV(_Device, ActiveProbeIndicesBuf.Get(), BufUav, ActiveProbeIndicesUAVSlot);
        BufUav.Buffer.NumElements = 1;
        _SRVHeap.CreateUAV(_Device, ActiveProbeCountBuf.Get(), BufUav, ActiveProbeCountUAVSlot);
        BufUav.Buffer.NumElements = 3;
        _SRVHeap.CreateUAV(_Device, ActiveProbeDispatchArgsBuf.Get(), BufUav,
                           ActiveProbeDispatchArgsUAVSlot);

        D3D12_CPU_DESCRIPTOR_HANDLE Src[8] = {
            _SRVHeap.CpuHandleStaging(_TlasSRVSlot),
            _SRVHeap.CpuHandleStaging(_SkyViewSRVSlot),
            _SRVHeap.CpuHandleStaging(_InstanceGeoSRVSlot),
            _SRVHeap.CpuHandleStaging(AtlasSRVSlot),
            // t4 = atlas de distancia; t5 preserva o layout compartilhado de descritores.
            _SRVHeap.CpuHandleStaging(DistSRVSlot),
            _SRVHeap.CpuHandleStaging(_InstanceGeoSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeRayCountSRVSlot),
        };
        UINT DstCount = 8; UINT SrcCounts[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
        for (u32 i = 0; i < kTraceTables; ++i) {
            TraceTable[i] = _SRVHeap.Allocate(11);
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[i]);
            _Device->CopyDescriptors(1, &Dst, &DstCount, 8, Src, SrcCounts,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            _Device->CopyDescriptorsSimple(
                1, _SRVHeap.CpuHandle(TraceTable[i] + 9),
                _SRVHeap.CpuHandleStaging(ActiveProbeIndicesSRVSlot),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            _Device->CopyDescriptorsSimple(
                1, _SRVHeap.CpuHandle(TraceTable[i] + 10),
                _SRVHeap.CpuHandleStaging(ActiveProbeCountSRVSlot),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        SceneGITableStart_ = _SRVHeap.Allocate(3);
        D3D12_CPU_DESCRIPTOR_HANDLE GDst = _SRVHeap.CpuHandle(SceneGITableStart_);
        D3D12_CPU_DESCRIPTOR_HANDLE GSrc[3] = {
            _SRVHeap.CpuHandleStaging(AtlasSRVSlot),
            _SRVHeap.CpuHandleStaging(DistSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
        };
        UINT GDstCount = 3; UINT GSrcCounts[3] = { 1, 1, 1 };
        _Device->CopyDescriptors(1, &GDst, &GDstCount, 3, GSrc, GSrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        UpdateTableStart = _SRVHeap.Allocate(4);
        D3D12_CPU_DESCRIPTOR_HANDLE UDst = _SRVHeap.CpuHandle(UpdateTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE USrc[4] = {
            _SRVHeap.CpuHandleStaging(ProbesTraceSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
            _SRVHeap.CpuHandleStaging(ActiveProbeIndicesSRVSlot),
            _SRVHeap.CpuHandleStaging(ActiveProbeCountSRVSlot),
        };
        UINT UDstCount = 4; UINT USrcCounts[4] = { 1, 1, 1, 1 };
        _Device->CopyDescriptors(1, &UDst, &UDstCount, 4, USrc, USrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        const FCascade& Coarse = Cascades[CoarseCascade()];
        CPU.GridMinSpacing  = { Coarse.GridMin.X, Coarse.GridMin.Y, Coarse.GridMin.Z,
                                Coarse.Spacing };
        CPU.GridCountRays   = { (f32)CountX, (f32)CountY, (f32)CountZ, (f32)kRaysPerProbe };
        CPU.AtlasParams     = { (f32)kTileSize, (f32)AtlasWidth, (f32)AtlasHeight, (f32)NumProbes };
        CPU.DistAtlasParams = { (f32)kDistTileSize, (f32)DistAtlasWidth, (f32)DistAtlasHeight, 0.0f };

        _Queue.ResetForRecording();
        ID3D12GraphicsCommandList* CL = _Queue.List();
        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CL->SetDescriptorHeaps(1, Heaps);

        Transition(CL, IrradAtlas.Get(), AtlasState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, DistAtlas.Get(),  DistState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float Zero[4] = { 0, 0, 0, 0 };
        CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(AtlasUAVSlot),
                                          _SRVHeap.CpuHandleStaging(AtlasUAVSlot),
                                          IrradAtlas.Get(), Zero, 0, nullptr);
        CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(DistUAVSlot),
                                          _SRVHeap.CpuHandleStaging(DistUAVSlot),
                                          DistAtlas.Get(), Zero, 0, nullptr);

        Transition(CL, ProbeDataBuf.Get(), ProbeDataState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CL->ClearUnorderedAccessViewFloat(_SRVHeap.GpuHandle(ProbeDataUAVSlot),
                                          _SRVHeap.CpuHandleStaging(ProbeDataUAVSlot),
                                          ProbeDataBuf.Get(), Zero, 0, nullptr);

        Transition(CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT RayCountInit[4] = { 64, 64, 64, 64 };
        CL->ClearUnorderedAccessViewUint(_SRVHeap.GpuHandle(ProbeRayCountUAVSlot),
                                         _SRVHeap.CpuHandleStaging(ProbeRayCountUAVSlot),
                                         ProbeRayCountBuf.Get(), RayCountInit, 0, nullptr);
        Transition(CL, ActiveProbeIndicesBuf.Get(), ActiveProbeIndicesState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, ActiveProbeCountBuf.Get(), ActiveProbeCountState,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT CountZero[4] = { 0, 0, 0, 0 };
        CL->ClearUnorderedAccessViewUint(_SRVHeap.GpuHandle(ActiveProbeCountUAVSlot),
                                         _SRVHeap.CpuHandleStaging(ActiveProbeCountUAVSlot),
                                         ActiveProbeCountBuf.Get(), CountZero, 0, nullptr);
        Transition(CL, ActiveProbeCountBuf.Get(), ActiveProbeCountState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, IrradAtlas.Get(),   AtlasState,     kAtlasRead);
        Transition(CL, DistAtlas.Get(),    DistState,      kAtlasRead);
        Transition(CL, ProbeDataBuf.Get(), ProbeDataState, kAtlasRead); 
        Transition(CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL };
        _Queue.ExecuteAndSync(Lists, 1);

        Ready = true;
        // Atlas recem-limpos devem substituir, nao misturar com, o historico zerado.
        HysteresisResetPending = true;
        RelocateFramesLeft = Relocation ? kRelocateConvergeFrames
                                        : (AdaptiveRays ? kReclassifyFrames : 0);
        LastProbeWakeSerial_ = 0;
        LastActiveProbeCount_ = NumProbes;
        LastCompactedProbeCapacity_ = NumProbes;
        ProbeCompactionThisUpdate_ = false;
        LastUpdateUsedProbeCompaction_ = false;
        for (u32 I = 0; I < kTraceTables; ++I) {
            CompactionReadbackIssued_[I] = false;
            CompactionReadbackCapacity_[I] = 0;
        }
        // O estado regional pertence ao volume descartado e nao atravessa o rebuild.
        InvalidateFramesLeft_     = 0;
        InvalidateDistFramesLeft_ = 0;
        InvalidateMin_            = {};
        InvalidateMax_            = {};
        ReclassifyPending_        = false;
        ScrollFollowUpPending     = false;
        LogDebug("[GI] - DDGI volume: " + std::to_string(CountX) + "x" + std::to_string(CountY) +
                "x" + std::to_string(CountZ) + " probes (" + std::to_string(NumProbes) +
                "), spacing " + std::to_string(Cascades[0].Spacing) + ", atlas " +
                std::to_string(AtlasWidth) + "x" + std::to_string(AtlasHeight));
    }

    void FDDGI::SetPunctualLightsSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _StagingSlot, u32 _FrameSlot) {
        static_assert(kTraceTables == FCommandQueue::kFramesInFlight,
                      "tabela de trace versionada por frame em voo");
        if (!Ready) return;
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[_FrameSlot] + 8);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_StagingSlot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FDDGI::PrepareCascadePlacement(const Vec3& _CameraPos) {
        if (!Ready || CascadeCount_ <= 1) return;

        // As cascatas finas seguem a camera; a grossa permanece alinhada à cena.
        const u32 Coarse = CoarseCascade();
        for (u32 C = 0; C < Coarse; ++C) {
            f32 Spacing = Cascades[Coarse].Spacing;
            for (u32 Step = Coarse; Step > C; --Step) Spacing /= kCascadeSpacingRatio;

            const Vec3 Center = _CameraPos;
            // Snap em celulas inteiras mantem o historico toroidal estavel.
            auto SnapCells = [&](f32 CenterAxis, int Count) {
                const f32 Extent = 0.5f * static_cast<f32>(Count - 1) * Spacing;
                return static_cast<int>(std::floor((CenterAxis - Extent) / Spacing));
            };
            const int Cells[3] = { SnapCells(Center.X, CountX),
                                   SnapCells(Center.Y, CountY),
                                   SnapCells(Center.Z, CountZ) };
            const int Counts[3] = { CountX, CountY, CountZ };

            Cascades[C].Spacing = Spacing;
            for (int A = 0; A < 3; ++A) {
                Cascades[C].OriginCells[A] = Cells[A];
                // O resto do C++ pode ser negativo; coordenadas de armazenamento nao.
                const int N = Counts[A] > 0 ? Counts[A] : 1;
                Cascades[C].Scroll[A] = ((Cells[A] % N) + N) % N;
            }
            Cascades[C].GridMin = { static_cast<f32>(Cells[0]) * Spacing,
                                    static_cast<f32>(Cells[1]) * Spacing,
                                    static_cast<f32>(Cells[2]) * Spacing };
        }
    }

    void FDDGI::ScheduleUpdate() {
        ScheduledFullForced_ = false;
        ScheduledCascadeCount_ = CascadeCount_;

        // O layout atual atualiza um prefixo; apenas duas cascatas aceitam intercalacao.
        if (InterleavedUpdates && CascadeCount_ == 2) {
            // Substituir toda a cascata fina força um update sincronizado da grossa.
            const int Counts[3] = { CountX, CountY, CountZ };
            bool FullFineReplacement = false;
            for (u32 C = 0; C < CoarseCascade() && !FullFineReplacement; ++C) {
                for (int A = 0; A < 3; ++A) {
                    const int Delta = Cascades[C].OriginCells[A] - Cascades[C].PrevOriginCells[A];
                    if (std::abs(Delta) >= std::max(Counts[A], 1)) {
                        FullFineReplacement = true;
                        break;
                    }
                }
            }
            const bool Urgent = HysteresisResetPending || InvalidateFramesLeft_ > 0 ||
                                InvalidateDistFramesLeft_ > 0 || RelocateFramesLeft > 0 ||
                                ReclassifyPending_ || FullFineReplacement;
            ScheduledFullForced_ = Urgent;
            ScheduledCascadeCount_ = (Urgent || CoarseDue_) ? 2u : 1u;
        }
        ScheduledProbeCount_ = ProbesPerCascade_ * ScheduledCascadeCount_;
    }

    void FDDGI::UpdatePerFrame(u32 _FrameSlot, const Vec3& _DirToSun, f32 _SunIntensity,
                               const Vec3& _SunColor, u32 _FrameIndex, u32 _PunctualLightCount) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;

        // Reutilizar o slot garante que a leitura assincrona da contagem terminou.
        if (CompactionReadbackIssued_[FrameSlot] && MappedActiveProbeCount) {
            u32 Count = 0;
            std::memcpy(&Count, MappedActiveProbeCount +
                        static_cast<size_t>(FrameSlot) * sizeof(u32), sizeof(u32));
            LastCompactedProbeCapacity_ = CompactionReadbackCapacity_[FrameSlot];
            LastActiveProbeCount_ = std::min(Count, LastCompactedProbeCapacity_);
            CompactionReadbackIssued_[FrameSlot] = false;
        }

        // Dois updates periodicos permitem reativar sondas caso um evento de geometria escape.
        if (ProbeCompaction && RelocateFramesLeft == 0 &&
            UpdateSerial_ >= LastProbeWakeSerial_ + kProbeWakeInterval) {
            RelocateFramesLeft = kProbeWakeUpdates;
            LastProbeWakeSerial_ = UpdateSerial_;
        }
        ScheduleUpdate();
        ProbeCompactionThisUpdate_ = ProbeCompaction && DispatchCommandSignature &&
                                     RelocateFramesLeft == 0 && !HysteresisResetPending;
        CPU.SunDirIntensity = { _DirToSun.X, _DirToSun.Y, _DirToSun.Z, _SunIntensity };
        CPU.SunColorHyst    = { _SunColor.X, _SunColor.Y, _SunColor.Z,
                                HysteresisResetPending ? 0.0f : Hysteresis };
        CPU.DistAtlasParams.W = HysteresisResetPending ? 0.0f : kDistHysteresis;
        const bool Invalidating = InvalidateFramesLeft_ > 0;
        // A folga da grossa alcança sondas vizinhas de todas as cascatas.
        const f32 Pad = Spacing();
        CPU.InvalidateMin     = { InvalidateMin_.X - Pad, InvalidateMin_.Y - Pad,
                                  InvalidateMin_.Z - Pad, Invalidating ? 1.0f : 0.0f };
        CPU.InvalidateMaxHyst = { InvalidateMax_.X + Pad, InvalidateMax_.Y + Pad,
                                  InvalidateMax_.Z + Pad, kInvalidateHysteresis };
        CPU.Cascades = CascadeConstants();
        // O prefixo agendado independe da capacidade total em AtlasParams.w.
        CPU.Cascades.Params.Z = static_cast<f32>(ScheduledProbeCount_);
        // Preserva a meia-vida temporal quando a grossa pula um update intercalado.
        CPU.Cascades.Params.W = static_cast<f32>(
            (InterleavedUpdates && CascadeCount_ == 2 && ScheduledCascadeCount_ == 2)
                ? std::min(CascadeUpdateAge_[CoarseCascade()] + 1u, 2u)
                : 1u);
        const int Counts[3] = { CountX, CountY, CountZ };
        for (u32 i = 0; i < kMaxCascades; ++i) {
            const FCascade& Cs = Cascades[i < CascadeCount_ ? i : CoarseCascade()];
            f32  D[3]      = { 0.0f, 0.0f, 0.0f };
            bool Scrolled  = false;
            for (int A = 0; A < 3; ++A) {
                const int Raw   = Cs.OriginCells[A] - Cs.PrevOriginCells[A];
                const int Limit = Counts[A] + 1;
                D[A] = static_cast<f32>(std::clamp(Raw, -Limit, Limit));
                if (Raw != 0) Scrolled = true;
            }
            CPU.CascadeScrollDelta[i] = { D[0], D[1], D[2], Scrolled ? 1.0f : 0.0f };
        }
        // A estreia controla novas marcas; o trabalho inclui o follow-up apos a relocacao.
        const bool ScrollDebut = ScrolledSinceLastUpdate();
        const bool ScrollWork  = ScrollDebut || ScrollFollowUpPending;
        const bool RelocRuns   = RelocateFramesLeft > 0 || ScrollWork;
        const bool ScrollOnly  = RelocateFramesLeft == 0 && ScrollWork;
        CPU.MiscParams3       = { kInvalidateDistHysteresis,
                                  InvalidateDistFramesLeft_ > 0 ? 1.0f : 0.0f,
                                  RelocRuns ? 1.0f : 0.0f, ScrollOnly ? 1.0f : 0.0f };
        CPU.TraceParams     = { (f32)_FrameIndex, MaxRayDist, SkyIntensity,
                                RayEps.HitShadowRayBias };
        CPU.RayEpsA         = { RayEps.OriginFloorMin, RayEps.OriginFloorPerMeter,
                                RayEps.OriginAngularMax, RayEps.ShadowRayBiasMin };
        CPU.RayEpsB         = { RayEps.ShadowRayTMin, RayEps.VisRayTMin, RayEps.VisRayEndMargin,
                                FRayEpsilonProfile::kOriginAngularMinRatio };
        CPU.GIDistParams    = { GIHit.DistTile, GIHit.DistAtlasW, GIHit.DistAtlasH,
                                GIHit.SkipModePacked() };
        CPU.GIBiasParams    = { GIHit.BiasScale, GIHit.BiasMax, GIHit.FadeProbes,
                                GIHit.SecondaryRoughnessMin };
        CPU.ReGIRGridMinSlots     = ReGIRParams.GridMinSlots;
        CPU.ReGIRInvCellEnabled   = ReGIRParams.InvCellSizeEnabled;
        CPU.ReGIRGridCountSamples = ReGIRParams.GridCountSamples;
        CPU.ReGIRResources        = ReGIRParams.Resources;
        CPU.RadianceCacheCamCell     = RadianceCacheParams.CameraPosCell;
        CPU.RadianceCacheLodCapFlags = RadianceCacheParams.LodCapacityFlags;
        CPU.RadianceCacheResources   = RadianceCacheParams.Resources;
        CPU.SkyParams             = SkyLutParams;

        const f32 EffMax = AdaptiveRays ? (f32)MaxRays : (f32)kRaysPerProbe;
        const f32 EffMin = AdaptiveRays ? (f32)std::min(MinRays, MaxRays) : (f32)kRaysPerProbe;
        CPU.MiscParams      = { Relocation ? 1.0f : 0.0f, DeactivationThreshold, EffMax, EffMin };
        // Uma marca de relocacao so nasce se um passe posterior puder consumi-la.
        CPU.MiscParams2     = { (Relocation && (RelocateFramesLeft > 1 || ScrollDebut)) ? 1.0f : 0.0f,
                                static_cast<f32>(_PunctualLightCount),
                                static_cast<f32>(FoliageShadows ? kRTMaskShadowFull
                                                                : kRTMaskShadowFast),
                                AdaptiveHysteresis ? 1.0f : 0.0f };
        CPU.ProbeCompactionParams = { ProbeCompactionThisUpdate_ ? 1.0f : 0.0f,
                                      0.0f, 0.0f, 0.0f };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(DDGIConstants),
                    &CPU, sizeof(DDGIConstants));
    }

    void FDDGI::Transition(ID3D12GraphicsCommandList* _CL, ID3D12Resource* _Res,
                           D3D12_RESOURCE_STATES& _State, D3D12_RESOURCE_STATES _After) {
        if (_State == _After) return;
        D3D12_RESOURCE_BARRIER B{};
        B.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource   = _Res;
        B.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore = _State;
        B.Transition.StateAfter  = _After;
        _CL->ResourceBarrier(1, &B);
        _State = _After;
    }

    void FDDGI::TransitionForUpdate(ID3D12GraphicsCommandList* _CL) {
        if (!Ready) return;
        // A fila direta remove PIXEL; o trace ainda precisa dos atlas como SRVs de compute.
        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(_CL, IrradAtlas.Get(),  AtlasState,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, DistAtlas.Get(),   DistState,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, ProbeDataBuf.Get(), ProbeDataState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void FDDGI::TransitionForRead(ID3D12GraphicsCommandList* _CL) {
        if (!Ready) return;
        Transition(_CL, IrradAtlas.Get(),   AtlasState,     kAtlasRead);
        Transition(_CL, DistAtlas.Get(),    DistState,      kAtlasRead);
        Transition(_CL, ProbeDataBuf.Get(), ProbeDataState, kAtlasRead);
    }

    void FDDGI::RecordUpdate(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;
        HysteresisResetPending = false;
        // Janelas de invalidacao contam updates executados, nao frames renderizados.
        if (InvalidateFramesLeft_ > 0)     --InvalidateFramesLeft_;
        if (InvalidateDistFramesLeft_ > 0) --InvalidateDistFramesLeft_;

        // Captura o scroll apenas apos executar o update para nao perder laminas expostas.
        const bool ScrollDebut = ScrolledSinceLastUpdate();
        const bool ScrolledNow = ScrollDebut || ScrollFollowUpPending;
        ScrollFollowUpPending = ScrollDebut;
        for (u32 C = 0; C < CascadeCount_; ++C)
            for (int A = 0; A < 3; ++A)
                Cascades[C].PrevOriginCells[A] = Cascades[C].OriginCells[A];

        // Os três passes pesados compartilham esta grade 2D de uma sonda por grupo.
        const u32 UpdateProbes = std::max(ScheduledProbeCount_, 1u);
        const u32 GroupsX = DispatchGroupsX(UpdateProbes);
        const u32 GroupsY = DispatchGroupsY(UpdateProbes);

        const bool UseCompaction = ProbeCompactionThisUpdate_ && DispatchCommandSignature &&
                                   ActiveProbeIndicesBuf && ActiveProbeCountBuf &&
                                   ActiveProbeDispatchArgsBuf;
        LastUpdateUsedProbeCompaction_ = UseCompaction;

        auto UAVBarrier = [&](ID3D12Resource* Resource = nullptr) {
            D3D12_RESOURCE_BARRIER B{};
            B.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            B.UAV.pResource = Resource;
            _CL->ResourceBarrier(1, &B);
        };
        auto DispatchProbeWork = [&] {
            if (UseCompaction) {
                _CL->ExecuteIndirect(DispatchCommandSignature.Get(), 1,
                                     ActiveProbeDispatchArgsBuf.Get(), 0, nullptr, 0);
            } else {
                _CL->Dispatch(GroupsX, GroupsY, 1);
            }
        };

        if (UseCompaction) {
            Transition(_CL, ActiveProbeIndicesBuf.Get(), ActiveProbeIndicesState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(_CL, ActiveProbeCountBuf.Get(), ActiveProbeCountState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(_CL, ActiveProbeDispatchArgsBuf.Get(), ActiveProbeDispatchArgsState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            const UINT Zero[4] = { 0, 0, 0, 0 };
            _CL->ClearUnorderedAccessViewUint(
                _SRVHeap.GpuHandle(ActiveProbeCountUAVSlot),
                _SRVHeap.CpuHandleStaging(ActiveProbeCountUAVSlot),
                ActiveProbeCountBuf.Get(), Zero, 0, nullptr);

            CompactBuildPSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ProbeDataSRVSlot));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ActiveProbeBuildUAVStart));
            _CL->Dispatch((UpdateProbes + 63) / 64, 1, 1);
            UAVBarrier(); // publica lista e contagem antes do finalize e dos consumidores

            Transition(_CL, ActiveProbeCountBuf.Get(), ActiveProbeCountState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CompactFinalizePSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ActiveProbeCountSRVSlot));
            _CL->SetComputeRootDescriptorTable(2,
                _SRVHeap.GpuHandle(ActiveProbeDispatchArgsUAVSlot));
            _CL->Dispatch(1, 1, 1);
            UAVBarrier(ActiveProbeDispatchArgsBuf.Get());

            Transition(_CL, ActiveProbeIndicesBuf.Get(), ActiveProbeIndicesState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(_CL, ActiveProbeDispatchArgsBuf.Get(), ActiveProbeDispatchArgsState,
                       D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }

        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbesTraceUAVSlot));
        DispatchProbeWork();

        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, IrradAtlas.Get(),  AtlasState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        UpdatePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(UpdateTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AtlasUAVSlot));
        DispatchProbeWork();

        Transition(_CL, DistAtlas.Get(), DistState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        UpdateDistPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(UpdateTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DistUAVSlot));
        DispatchProbeWork();

        if (RelocateFramesLeft > 0 || ScrolledNow) {
            if (RelocateFramesLeft > 0) --RelocateFramesLeft;
            Transition(_CL, ProbeDataBuf.Get(),     ProbeDataState,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            RelocatePSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ProbesTraceSRVSlot));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbeDataUAVSlot)); 
            _CL->Dispatch((UpdateProbes + 63) / 64, 1, 1);
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        if (UseCompaction) {
            // Telemetria assincrona; restaura SRV antes de reutilizar descritores permanentes.
            Transition(_CL, ActiveProbeCountBuf.Get(), ActiveProbeCountState,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            _CL->CopyBufferRegion(
                ActiveProbeCountReadback.Get(),
                static_cast<UINT64>(FrameSlot) * sizeof(u32),
                ActiveProbeCountBuf.Get(), 0, sizeof(u32));
            Transition(_CL, ActiveProbeCountBuf.Get(), ActiveProbeCountState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CompactionReadbackIssued_[FrameSlot] = true;
            CompactionReadbackCapacity_[FrameSlot] = UpdateProbes;
        }

        // Agenda apos o cbuffer deste frame para o proximo trace usar todos os raios.
        if (ReclassifyPending_ && InvalidateFramesLeft_ == 0 && InvalidateDistFramesLeft_ == 0) {
            ReclassifyPending_ = false;
            if (Relocation || AdaptiveRays) TriggerReclassify();
        }

        LastUpdatedCascadeCount_ = ScheduledCascadeCount_;
        for (u32 C = 0; C < CascadeCount_; ++C) {
            if (C < ScheduledCascadeCount_) CascadeUpdateAge_[C] = 0;
            else if (CascadeUpdateAge_[C] < 0xFFFFFFFFu) ++CascadeUpdateAge_[C];
        }
        ++UpdateSerial_;
        if (ScheduledFullForced_) LastForcedUpdateSerial_ = UpdateSerial_;
        CoarseDue_ = ScheduledCascadeCount_ < CascadeCount_;
    }

    void FDDGI::CreatePipelines(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "DDGITrace.cs_6_6.cso", 11, 1, true);
        UpdatePSO.Initialize(_Device, "DDGIUpdate.cs_6_0.cso", 4, 1);
        UpdateDistPSO.Initialize(_Device, "DDGIUpdateDist.cs_6_0.cso", 4, 1);
        RelocatePSO.Initialize(_Device, "DDGIRelocate.cs_6_0.cso", 1, 2);
        CompactBuildPSO.Initialize(_Device, "DDGICompactBuild.cs_6_0.cso", 1, 2);
        CompactFinalizePSO.Initialize(_Device, "DDGICompactFinalize.cs_6_0.cso", 1, 1);

        D3D12_INDIRECT_ARGUMENT_DESC Arg{};
        Arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC Desc{};
        Desc.ByteStride       = sizeof(D3D12_DISPATCH_ARGUMENTS);
        Desc.NumArgumentDescs = 1;
        Desc.pArgumentDescs   = &Arg;
        SMILE_HR(_Device->CreateCommandSignature(
            &Desc, nullptr, IID_PPV_ARGS(&DispatchCommandSignature)));
    }


    FPassShaderStems FDDGI::ShaderStems() const {
        static const char* const kStems[] = {
            "DDGITrace.cs", "DDGIUpdate.cs", "DDGIUpdateDist.cs", "DDGIRelocate.cs",
            "DDGICompactBuild.cs", "DDGICompactFinalize.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FDDGI::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
