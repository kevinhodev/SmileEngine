#include "Smile/Graphics/DDGI.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Scene/Scene.h"
#include "Smile/Graphics/Material.h"
#include "Smile/Graphics/Mesh.h"
#include "Smile/Core/HResultCheck.h"
#include "Smile/Core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr DXGI_FORMAT kAtlasFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

        ComPtr<ID3D12Resource> CreateTex2D(ID3D12Device* _Device, u32 _W, u32 _H,
                                           DXGI_FORMAT _Fmt) {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            Desc.Width            = _W;
            Desc.Height           = _H;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = _Fmt;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> Tex;
            SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Tex)));
            VramTracker::Register(Tex.Get(), EVramCategory::GI);
            return Tex;
        }

        // Espelho C++ do InstanceGeo de DDGICommon.hlsli (80 bytes) — mudar em lockstep.
        // Flags: 1=AlphaTest (FORCE_NON_OPAQUE na TLAS), 2=HasEmissiveMap, 4=Foliage, 8=HasMrMap.
        struct DDGIInstanceGeo {
            Vec4 BaseColor;
            u32  VertexSrv   = 0; // indice bindless (ResourceDescriptorHeap) do VB do mesh
            u32  IndexSrv    = 0; // idem p/ o IB — buffers 0-based por mesh, sem offsets
            u32  AlbedoIndex = 0;
            u32  HasAlbedo   = 0;
            u32  TwoSided    = 0;
            u32  Flags       = 0;
            f32  AlphaCutoff = 0.5f;
            f32  RoughnessFactor = 0.5f;
            Vec4 EmissiveFactor{ 0.0f, 0.0f, 0.0f, 0.0f }; // rgb = fator*strength; w = MetallicFactor
            u32  EmissiveMapIndex = 0;
            u32  MrMapIndex       = 0;
            u32  GeoPad0 = 0, GeoPad1 = 0;
        };
        static_assert(sizeof(DDGIInstanceGeo) == 80, "DDGIInstanceGeo deve casar com o HLSL (80B)");

        ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* _Device, UINT64 _Size,
                                                   D3D12_RESOURCE_STATES _State,
                                                   D3D12_RESOURCE_FLAGS _Flags = D3D12_RESOURCE_FLAG_NONE) {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_DEFAULT;
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
            ComPtr<ID3D12Resource> Buf;
            SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     _State, nullptr, IID_PPV_ARGS(&Buf)));
            VramTracker::Register(Buf.Get(), EVramCategory::GI);
            return Buf;
        }

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* _Device, UINT64 _Size,
                                                  u8** _MappedOut) {
            D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC Desc{};
            Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            Desc.Width            = _Size;
            Desc.Height           = 1;
            Desc.DepthOrArraySize = 1;
            Desc.MipLevels        = 1;
            Desc.Format           = DXGI_FORMAT_UNKNOWN;
            Desc.SampleDesc       = { 1, 0 };
            Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> Buf;
            SMILE_HR(_Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buf)));
            if (_MappedOut) {
                D3D12_RANGE NoRead{ 0, 0 };
                SMILE_HR(Buf->Map(0, &NoRead, reinterpret_cast<void**>(_MappedOut)));
            }
            return Buf;
        }
    }

    void FDDGI::Initialize(ID3D12Device* _Device) {
        TracePSO.Initialize(_Device, "DDGITrace.cs_6_6.cso", 9, 1, true); // t8 = luzes (F5)
        UpdatePSO.Initialize(_Device, "DDGIUpdate.cs_6_0.cso", 2, 1);
        UpdateDistPSO.Initialize(_Device, "DDGIUpdateDist.cs_6_0.cso", 2, 1);
        RelocatePSO.Initialize(_Device, "DDGIRelocate.cs_6_0.cso", 1, 2);
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
        FreeSlot(InstanceSRVSlot, 1);
        if (MeshGeoSlotBase != kInvalidSlot) {
            _SRVHeap.Free(MeshGeoSlotBase, MeshGeoSlotCount);
            MeshGeoSlotBase  = kInvalidSlot;
            MeshGeoSlotCount = 0;
        }
        FreeSlot(ProbeDataSRVSlot, 1);
        FreeSlot(ProbeRayCountSRVSlot, 1);
        FreeSlot(ProbeDataUAVSlot, 2); 
        ProbeRayCountUAVSlot = kInvalidSlot;
        for (u32 i = 0; i < kTraceTables; ++i) FreeSlot(TraceTable[i], 9);
        FreeSlot(SceneGITableStart_, 3);
        FreeSlot(UpdateTableStart, 2);
        IrradAtlas.Reset();
        DistAtlas.Reset();
        ProbesTrace.Reset();
        InstanceGeoBuf.Reset();
        ProbeDataBuf.Reset();
        ProbeRayCountBuf.Reset();
        AtlasState         = D3D12_RESOURCE_STATE_COMMON;
        DistState          = D3D12_RESOURCE_STATE_COMMON;
        ProbesState        = D3D12_RESOURCE_STATE_COMMON;
        ProbeDataState     = D3D12_RESOURCE_STATE_COMMON;
        ProbeRayCountState = D3D12_RESOURCE_STATE_COMMON;
        Ready = false;
    }

    void FDDGI::SetupForScene(ID3D12Device* _Device, FCommandQueue& _Queue,
                              FTextureSRVHeap& _SRVHeap, const FScene& _Scene,
                              const Vec3& _AABBMin, const Vec3& _AABBMax,
                              u32 _TlasSRVSlot, u32 _SkyViewSRVSlot) {
        if (!Initialized) return;
        ReleaseSceneResources(_SRVHeap);

        const u32 NumRenderables = static_cast<u32>(_Scene.Renderables().size());
        if (NumRenderables == 0 || _TlasSRVSlot == kInvalidSlot) {
            LogWarning("[GI] - DDGI: Cena sem Geometria/TLAS; Volume nao Criado");
            return;
        }

        Vec3 ext = { std::max(_AABBMax.X - _AABBMin.X, 0.1f),
                     std::max(_AABBMax.Y - _AABBMin.Y, 0.1f),
                     std::max(_AABBMax.Z - _AABBMin.Z, 0.1f) };
        f32 maxExt = std::max(ext.X, std::max(ext.Y, ext.Z));
        const int kTargetMax = 24, kMaxPerAxis = 32;
        SpacingV = std::max(maxExt / (kTargetMax - 1), 0.5f);
        auto axisCount = [&](f32 e) {
            int n = static_cast<int>(std::ceil(e / SpacingV)) + 1;
            return std::clamp(n, 2, kMaxPerAxis);
        };
        CountX = axisCount(ext.X); CountY = axisCount(ext.Y); CountZ = axisCount(ext.Z);
        GridMinV = { _AABBMin.X - 0.5f * SpacingV, _AABBMin.Y - 0.5f * SpacingV,
                     _AABBMin.Z - 0.5f * SpacingV };
        NumProbes       = static_cast<u32>(CountX) * CountY * CountZ;
        // Tiles com 1px de borda octaedrica de cada lado (stride = tile+2; ver DDGI_TileOrigin).
        // Pior caso 32x32x32: 1024 tiles * 16px = 16384 = limite exato de textura do D3D12.
        AtlasWidth      = static_cast<u32>(CountX) * CountZ * (kTileSize + 2);
        AtlasHeight     = static_cast<u32>(CountY) * (kTileSize + 2);
        DistAtlasWidth  = static_cast<u32>(CountX) * CountZ * (kDistTileSize + 2);
        DistAtlasHeight = static_cast<u32>(CountY) * (kDistTileSize + 2);
        MaxRayDist      = std::sqrt(ext.X * ext.X + ext.Y * ext.Y + ext.Z * ext.Z) * 1.5f;

        IrradAtlas  = CreateTex2D(_Device, AtlasWidth, AtlasHeight, kAtlasFormat);
        DistAtlas   = CreateTex2D(_Device, DistAtlasWidth, DistAtlasHeight, DXGI_FORMAT_R16G16_FLOAT);
        ProbesTrace = CreateTex2D(_Device, static_cast<u32>(kRaysPerProbe), NumProbes, kAtlasFormat);

        // SRVs bindless de VB/IB por mesh único (2 slots contíguos: VB, IB) — o InstanceGeo
        // aponta pros índices e os shaders leem via ResourceDescriptorHeap (SM6.6). Substitui
        // os merged buffers, que duplicavam a geometria inteira da cena em VRAM.
        std::unordered_map<const FGpuMesh*, u32> MeshGeoSlot; // valor = slot do VB (IB = +1)
        std::vector<const FGpuMesh*> UniqueMeshes;
        for (u32 i = 0; i < NumRenderables; ++i) {
            const FGpuMesh* M = _Scene.Renderables()[i].Mesh;
            if (!M || !M->IsValid() || MeshGeoSlot.count(M)) continue;
            MeshGeoSlot[M] = 0;
            UniqueMeshes.push_back(M);
        }
        MeshGeoSlotCount = static_cast<u32>(UniqueMeshes.size()) * 2;
        MeshGeoSlotBase  = _SRVHeap.Allocate(MeshGeoSlotCount);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC GeoSrv{};
            GeoSrv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            GeoSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            for (u32 i = 0; i < static_cast<u32>(UniqueMeshes.size()); ++i) {
                const FGpuMesh* M      = UniqueMeshes[i];
                const u32       VbSlot = MeshGeoSlotBase + i * 2;
                // FirstElement = offset da fatia no pool de geometria (0 se buffer proprio)
                GeoSrv.Format                     = DXGI_FORMAT_UNKNOWN;
                GeoSrv.Buffer.FirstElement        = M->VertexFirstElement();
                GeoSrv.Buffer.NumElements         = M->VertexCount();
                GeoSrv.Buffer.StructureByteStride = sizeof(Vertex);
                _SRVHeap.CreateSRV(_Device, M->VertexResource(), GeoSrv, VbSlot);
                GeoSrv.Format                     = DXGI_FORMAT_R32_UINT;
                GeoSrv.Buffer.FirstElement        = M->IndexFirstElement();
                GeoSrv.Buffer.NumElements         = M->GetIndexCount();
                GeoSrv.Buffer.StructureByteStride = 0;
                _SRVHeap.CreateSRV(_Device, M->IndexResource(), GeoSrv, VbSlot + 1);
                MeshGeoSlot[M] = VbSlot;
            }
        }
        ProbeDataBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(Vec4),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ProbeRayCountBuf = CreateDefaultBuffer(_Device, static_cast<UINT64>(NumProbes) * sizeof(u32),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        u8* GeoMapped = nullptr;
        InstanceGeoBuf = CreateUploadBuffer(_Device,
            static_cast<UINT64>(NumRenderables) * sizeof(DDGIInstanceGeo), &GeoMapped);
        for (u32 i = 0; i < NumRenderables; ++i) {
            const FRenderable& R = _Scene.Renderables()[i];
            DDGIInstanceGeo g{};
            g.BaseColor = { 0.7f, 0.7f, 0.7f, 1.0f };
            if (R.Material) {
                const MaterialConstants& MC = R.Material->Constants;
                g.BaseColor = MC.BaseColorFactor;
                g.TwoSided  = R.Material->TwoSided ? 1u : 0u;
                if (R.Material->IsFinalized() && R.Material->HasAlbedoTexture()) {
                    g.AlbedoIndex = R.Material->AlbedoDescriptorIndex();
                    g.HasAlbedo   = 1;
                }
                // Campos p/ o ReSTIR PT (emissivo, alpha-test, metal/rough por instancia).
                g.AlphaCutoff     = MC.AlphaCutoff;
                g.RoughnessFactor = MC.RoughnessFactor;
                g.EmissiveFactor  = { MC.EmissiveFactor.X * MC.EmissiveStrength,
                                      MC.EmissiveFactor.Y * MC.EmissiveStrength,
                                      MC.EmissiveFactor.Z * MC.EmissiveStrength,
                                      MC.MetallicFactor };
                if (MC.AlphaTest)        g.Flags |= 1u;
                if (MC.ShadingModel == 1) g.Flags |= 4u; // Foliage
                if (R.Material->IsFinalized()) {
                    // Slots do material: 0=albedo, 1=normal, 2=metallic-roughness, 3=AO, 4=emissive.
                    if (MC.HasEmissiveMap) {
                        g.EmissiveMapIndex = R.Material->AlbedoDescriptorIndex() + 4;
                        g.Flags |= 2u;
                    }
                    if (MC.HasMetallicRoughnessMap) {
                        g.MrMapIndex = R.Material->AlbedoDescriptorIndex() + 2;
                        g.Flags |= 8u;
                    }
                }
            }
            auto It = R.Mesh ? MeshGeoSlot.find(R.Mesh) : MeshGeoSlot.end();
            if (It != MeshGeoSlot.end()) { g.VertexSrv = It->second; g.IndexSrv = It->second + 1; }
            std::memcpy(GeoMapped + i * sizeof(DDGIInstanceGeo), &g, sizeof(DDGIInstanceGeo));
        }
        InstanceGeoBuf->Unmap(0, nullptr);

        AtlasSRVSlot       = _SRVHeap.Allocate(1);
        AtlasUAVSlot       = _SRVHeap.Allocate(1);
        DistSRVSlot        = _SRVHeap.Allocate(1);
        DistUAVSlot        = _SRVHeap.Allocate(1);
        ProbesTraceSRVSlot = _SRVHeap.Allocate(1);
        ProbesTraceUAVSlot = _SRVHeap.Allocate(1);
        InstanceSRVSlot    = _SRVHeap.Allocate(1);
        ProbeDataSRVSlot     = _SRVHeap.Allocate(1);
        ProbeRayCountSRVSlot = _SRVHeap.Allocate(1);

        const u32 RelocUAVBase = _SRVHeap.Allocate(2);
        ProbeDataUAVSlot     = RelocUAVBase;
        ProbeRayCountUAVSlot = RelocUAVBase + 1;

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
        BufSrv.Format                     = DXGI_FORMAT_UNKNOWN;
        BufSrv.Buffer.FirstElement        = 0;
        BufSrv.Buffer.NumElements         = NumRenderables;
        BufSrv.Buffer.StructureByteStride = sizeof(DDGIInstanceGeo);
        _SRVHeap.CreateSRV(_Device, InstanceGeoBuf.Get(), BufSrv, InstanceSRVSlot);

        BufSrv.Format                     = DXGI_FORMAT_R32G32B32A32_FLOAT;
        BufSrv.Buffer.NumElements         = NumProbes;
        BufSrv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeDataBuf.Get(), BufSrv, ProbeDataSRVSlot);

        BufSrv.Format                     = DXGI_FORMAT_R32_UINT;
        BufSrv.Buffer.NumElements         = NumProbes;
        BufSrv.Buffer.StructureByteStride = 0;
        _SRVHeap.CreateSRV(_Device, ProbeRayCountBuf.Get(), BufSrv, ProbeRayCountSRVSlot);

        D3D12_UNORDERED_ACCESS_VIEW_DESC BufUav{};
        BufUav.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        BufUav.Format              = DXGI_FORMAT_R32G32B32A32_FLOAT;
        BufUav.Buffer.FirstElement = 0;
        BufUav.Buffer.NumElements  = NumProbes;
        _SRVHeap.CreateUAV(_Device, ProbeDataBuf.Get(), BufUav, ProbeDataUAVSlot);
        BufUav.Format              = DXGI_FORMAT_R32_UINT;
        _SRVHeap.CreateUAV(_Device, ProbeRayCountBuf.Get(), BufUav, ProbeRayCountUAVSlot);

        // t0..t7 fixos + t8 = luzes puntuais (F5; copiado por frame no SetPunctualLightsSRV).
        // Uma tabela por frame em voo: o t8 muda todo frame e a tabela do frame anterior ainda
        // pode estar sendo lida pela GPU (descriptor versioning). t4/t5 eram os merged VB/IB,
        // aposentados pelo bindless (InstanceGeo.VertexSrv/IndexSrv); recebem um descriptor
        // valido de enchimento p/ manter o layout da tabela (shader nao declara mais t4/t5).
        D3D12_CPU_DESCRIPTOR_HANDLE Src[8] = {
            _SRVHeap.CpuHandleStaging(_TlasSRVSlot),
            _SRVHeap.CpuHandleStaging(_SkyViewSRVSlot),
            _SRVHeap.CpuHandleStaging(InstanceSRVSlot),
            _SRVHeap.CpuHandleStaging(AtlasSRVSlot),
            _SRVHeap.CpuHandleStaging(InstanceSRVSlot),
            _SRVHeap.CpuHandleStaging(InstanceSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeRayCountSRVSlot),
        };
        UINT DstCount = 8; UINT SrcCounts[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
        for (u32 i = 0; i < kTraceTables; ++i) {
            TraceTable[i] = _SRVHeap.Allocate(9);
            D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[i]);
            _Device->CopyDescriptors(1, &Dst, &DstCount, 8, Src, SrcCounts,
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

        // Tabela dos passes de blend (Update/UpdateDist): t0 = ProbesTrace, t1 = ProbeData
        // (w>=1 = probe recem-ativado/relocado -> hysteresis 0 naquele frame).
        UpdateTableStart = _SRVHeap.Allocate(2);
        D3D12_CPU_DESCRIPTOR_HANDLE UDst = _SRVHeap.CpuHandle(UpdateTableStart);
        D3D12_CPU_DESCRIPTOR_HANDLE USrc[2] = {
            _SRVHeap.CpuHandleStaging(ProbesTraceSRVSlot),
            _SRVHeap.CpuHandleStaging(ProbeDataSRVSlot),
        };
        UINT UDstCount = 2; UINT USrcCounts[2] = { 1, 1 };
        _Device->CopyDescriptors(1, &UDst, &UDstCount, 2, USrc, USrcCounts,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CPU.GridMinSpacing  = { GridMinV.X, GridMinV.Y, GridMinV.Z, SpacingV };
        CPU.GridCountRays   = { (f32)CountX, (f32)CountY, (f32)CountZ, (f32)kRaysPerProbe };
        CPU.AtlasParams     = { (f32)kTileSize, (f32)AtlasWidth, (f32)AtlasHeight, (f32)NumProbes };
        CPU.DistAtlasParams = { (f32)kDistTileSize, (f32)DistAtlasWidth, (f32)DistAtlasHeight, 0.0f };

        _Queue.ResetForRecording();
        ID3D12GraphicsCommandList* CL = _Queue.List();
        ID3D12DescriptorHeap* Heaps[] = { _SRVHeap.Native() };
        CL->SetDescriptorHeaps(1, Heaps);

        // Sem copia de geometria: os shaders leem os VB/IB originais via bindless — os meshes
        // ja vivem em estado combinado de leitura que inclui NON_PIXEL (GpuMesh.cpp).
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
        Transition(CL, IrradAtlas.Get(),   AtlasState,     kAtlasRead);
        Transition(CL, DistAtlas.Get(),    DistState,      kAtlasRead);
        Transition(CL, ProbeDataBuf.Get(), ProbeDataState, kAtlasRead); 
        Transition(CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SMILE_HR(CL->Close());
        ID3D12CommandList* Lists[] = { CL };
        _Queue.ExecuteAndSync(Lists, 1);

        Ready = true;
        RelocateFramesLeft = Relocation ? kRelocateConvergeFrames : 0; 
        LogInfo("[GI] - DDGI volume: " + std::to_string(CountX) + "x" + std::to_string(CountY) +
                "x" + std::to_string(CountZ) + " probes (" + std::to_string(NumProbes) +
                "), spacing " + std::to_string(SpacingV) + ", atlas " +
                std::to_string(AtlasWidth) + "x" + std::to_string(AtlasHeight));
    }

    void FDDGI::SetPunctualLightsSRV(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _StagingSlot, u32 _FrameSlot) {
        static_assert(kTraceTables == FCommandQueue::kFramesInFlight,
                      "tabela de trace versionada por frame em voo");
        if (!Ready) return;
        // Escreve so na tabela DESTE FrameSlot: a outra pertence ao frame em voo.
        D3D12_CPU_DESCRIPTOR_HANDLE Dst = _SRVHeap.CpuHandle(TraceTable[_FrameSlot] + 8);
        D3D12_CPU_DESCRIPTOR_HANDLE Src = _SRVHeap.CpuHandleStaging(_StagingSlot);
        UINT One = 1;
        _Device->CopyDescriptors(1, &Dst, &One, 1, &Src, &One,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void FDDGI::UpdatePerFrame(u32 _FrameSlot, const Vec3& _DirToSun, f32 _SunIntensity,
                               const Vec3& _SunColor, u32 _FrameIndex, u32 _PunctualLightCount) {
        if (!Ready) return;
        FrameSlot = _FrameSlot;
        CPU.SunDirIntensity = { _DirToSun.X, _DirToSun.Y, _DirToSun.Z, _SunIntensity };
        CPU.SunColorHyst    = { _SunColor.X, _SunColor.Y, _SunColor.Z, Hysteresis };
        CPU.TraceParams     = { (f32)_FrameIndex, MaxRayDist, SkyIntensity, NormalBias };
        CPU.DistAtlasParams.W = RealHitShading ? 1.0f : 0.0f; 

        const f32 EffMax = AdaptiveRays ? (f32)MaxRays : 64.0f;
        const f32 EffMin = AdaptiveRays ? (f32)MinRays : 64.0f;
        CPU.MiscParams      = { Relocation ? 1.0f : 0.0f, DeactivationThreshold, EffMax, EffMin };
        // Marca de "recem-ativado" so quando o Relocate ainda tem >=1 frame agendado DEPOIS
        // deste (a marca precisa do proximo Relocate p/ o auto-demote; orfa = hyst 0 eterno).
        CPU.MiscParams2     = { (Relocation && RelocateFramesLeft > 1) ? 1.0f : 0.0f,
                                static_cast<f32>(_PunctualLightCount),
                                FoliageShadows ? 255.0f : 1.0f, 0.0f }; // z = ShadowRayMask
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

    void FDDGI::RecordUpdate(ID3D12GraphicsCommandList* _CL, FTextureSRVHeap& _SRVHeap) {
        if (!Ready) return;

        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TracePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TraceTable[FrameSlot]));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbesTraceUAVSlot));
        _CL->Dispatch(NumProbes, 1, 1);

        Transition(_CL, ProbesTrace.Get(), ProbesState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(_CL, IrradAtlas.Get(),  AtlasState,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        UpdatePSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(UpdateTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(AtlasUAVSlot));
        _CL->Dispatch(NumProbes, 1, 1);

        Transition(_CL, DistAtlas.Get(), DistState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        UpdateDistPSO.Bind(_CL);
        _CL->SetComputeRootConstantBufferView(0, CBAddr());
        _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(UpdateTableStart));
        _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(DistUAVSlot));
        _CL->Dispatch(NumProbes, 1, 1);

        if (RelocateFramesLeft > 0) {
            --RelocateFramesLeft;
            Transition(_CL, ProbeDataBuf.Get(),     ProbeDataState,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            RelocatePSO.Bind(_CL);
            _CL->SetComputeRootConstantBufferView(0, CBAddr());
            _CL->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(ProbesTraceSRVSlot));
            _CL->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ProbeDataUAVSlot)); 
            _CL->Dispatch((NumProbes + 63) / 64, 1, 1);
            Transition(_CL, ProbeDataBuf.Get(),     ProbeDataState,     kAtlasRead); 
            Transition(_CL, ProbeRayCountBuf.Get(), ProbeRayCountState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        Transition(_CL, IrradAtlas.Get(), AtlasState, kAtlasRead);
        Transition(_CL, DistAtlas.Get(),  DistState,  kAtlasRead);
    }
}
