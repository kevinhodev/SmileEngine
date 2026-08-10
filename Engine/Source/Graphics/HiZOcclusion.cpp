#include "Smile/Graphics/HiZOcclusion.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Barriers.h"
#include "Smile/Core/HResultCheck.h"
#include <algorithm>
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        u32 RoundUpPow2(u32 V) {
            u32 P = 1;
            while (P < V) P <<= 1;
            return P;
        }

        ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* _Device, u64 _Size,
                                            D3D12_HEAP_TYPE _Heap,
                                            D3D12_RESOURCE_STATES _State,
                                            D3D12_RESOURCE_FLAGS _Flags) {
            D3D12_HEAP_PROPERTIES Heap{};
            Heap.Type = _Heap;
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

    void FHiZOcclusion::Initialize(ID3D12Device* _Device) {
        CreatePipelines(_Device);
        Initialized = true;
    }

    void FHiZOcclusion::ReleaseSizedResources(FTextureSRVHeap& _SRVHeap) {
        for (u32 i = 0; i < kMaxMips; ++i) {
            if (MipSRVSlot[i] != kInvalidSlot) { _SRVHeap.Free(MipSRVSlot[i]); MipSRVSlot[i] = kInvalidSlot; }
            if (MipUAVSlot[i] != kInvalidSlot) { _SRVHeap.Free(MipUAVSlot[i]); MipUAVSlot[i] = kInvalidSlot; }
        }
        if (FullSRVSlot != kInvalidSlot) { _SRVHeap.Free(FullSRVSlot); FullSRVSlot = kInvalidSlot; }
        HZB.Reset();
        Ready = false;
        InvalidateResults(); // HZB novo: resultados antigos nao valem mais
    }

    void FHiZOcclusion::SetupForResize(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                       u32 _RenderW, u32 _RenderH) {
        if (!Initialized || _RenderW == 0 || _RenderH == 0) return;
        ReleaseSizedResources(_SRVHeap);

        SrcWidth  = _RenderW;
        SrcHeight = _RenderH;
        // Mip 0 = metade da render res arredondada pra cima em po2 (igual UE): os mips
        // halvam exato ate 1x1 e a matematica de footprint do teste fica trivial.
        HZBWidth  = std::max(RoundUpPow2(_RenderW) >> 1, 1u);
        HZBHeight = std::max(RoundUpPow2(_RenderH) >> 1, 1u);

        NumMips = 1;
        while (((HZBWidth >> NumMips) > 0 || (HZBHeight >> NumMips) > 0) && NumMips < kMaxMips)
            ++NumMips;

        HZB = GpuResources::CreateTex2D(
            _Device, HZBWidth, HZBHeight, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, EVramCategory::RenderTargets,
            nullptr, NumMips, 1, "HZB de oclusao");

        for (u32 m = 0; m < NumMips; ++m) {
            MipSRVSlot[m] = _SRVHeap.Allocate(1);
            MipUAVSlot[m] = _SRVHeap.Allocate(1);

            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                    = DXGI_FORMAT_R32_FLOAT;
            Srv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            Srv.Texture2D.MostDetailedMip = m;
            Srv.Texture2D.MipLevels       = 1;
            _SRVHeap.CreateSRV(_Device, HZB.Get(), Srv, MipSRVSlot[m]);

            D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
            Uav.Format             = DXGI_FORMAT_R32_FLOAT;
            Uav.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
            Uav.Texture2D.MipSlice = m;
            _SRVHeap.CreateUAV(_Device, HZB.Get(), Uav, MipUAVSlot[m]);
        }

        FullSRVSlot = _SRVHeap.Allocate(1);
        D3D12_SHADER_RESOURCE_VIEW_DESC FullSrv{};
        FullSrv.Format                    = DXGI_FORMAT_R32_FLOAT;
        FullSrv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        FullSrv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        FullSrv.Texture2D.MostDetailedMip = 0;
        FullSrv.Texture2D.MipLevels       = NumMips;
        _SRVHeap.CreateSRV(_Device, HZB.Get(), FullSrv, FullSRVSlot);

        Ready = true;
        RefreshTestTables(_Device, _SRVHeap);
    }

    void FHiZOcclusion::ReleaseObjectResources(FTextureSRVHeap& _SRVHeap) {
        for (u32 s = 0; s < kSlots; ++s) {
            if (TestTableSlot[s] != kInvalidSlot) { _SRVHeap.Free(TestTableSlot[s], 2); TestTableSlot[s] = kInvalidSlot; }
        }
        if (ResultUAVSlot != kInvalidSlot) { _SRVHeap.Free(ResultUAVSlot); ResultUAVSlot = kInvalidSlot; }
        if (BoundsBuffer && MappedBounds)     { BoundsBuffer->Unmap(0, nullptr);   MappedBounds = nullptr; }
        if (ReadbackBuffer && MappedReadback) { ReadbackBuffer->Unmap(0, nullptr); MappedReadback = nullptr; }
        if (TestCB && MappedTestCB)           { TestCB->Unmap(0, nullptr);         MappedTestCB = nullptr; }
        BoundsBuffer.Reset();
        ResultBuffer.Reset();
        ReadbackBuffer.Reset();
        TestCB.Reset();
        ObjectCapacity = 0;
        InvalidateResults();
    }

    void FHiZOcclusion::SetupObjects(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                     u32 _MaxObjects) {
        if (_MaxObjects == 0 || _MaxObjects == ObjectCapacity) return;
        ReleaseObjectResources(_SRVHeap);
        ObjectCapacity = _MaxObjects;

        const u64 BoundsSize = static_cast<u64>(kSlots) * ObjectCapacity * sizeof(FBoundsEntry);
        BoundsBuffer = CreateBuffer(_Device, BoundsSize, D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ,
                                    D3D12_RESOURCE_FLAG_NONE);
        D3D12_RANGE NoRead{ 0, 0 };
        void* P = nullptr;
        SMILE_HR(BoundsBuffer->Map(0, &NoRead, &P));
        MappedBounds = reinterpret_cast<u8*>(P);
        std::memset(MappedBounds, 0, BoundsSize); // Extent 0 = sempre visivel

        ResultBuffer = GpuResources::CreateBuffer(
            _Device, static_cast<u64>(ObjectCapacity) * sizeof(u32),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, EVramCategory::Misc,
            "Resultado do culling");
        ResultState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        ReadbackBuffer = CreateBuffer(_Device,
                                      static_cast<u64>(kSlots) * ObjectCapacity * sizeof(u32),
                                      D3D12_HEAP_TYPE_READBACK,
                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_FLAG_NONE);
        // Map persistente: heap READBACK aceita; a leitura por slot so acontece depois
        // do WaitForValue da fence do slot no BeginFrame (mesmo padrao do GpuProfiler).
        SMILE_HR(ReadbackBuffer->Map(0, nullptr, &P));
        MappedReadback = reinterpret_cast<const u32*>(P);

        TestCB = CreateBuffer(_Device, static_cast<u64>(kSlots) * 256,
                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                              D3D12_RESOURCE_FLAG_NONE);
        SMILE_HR(TestCB->Map(0, &NoRead, &P));
        MappedTestCB = reinterpret_cast<u8*>(P);

        ResultUAVSlot = _SRVHeap.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format                      = DXGI_FORMAT_UNKNOWN;
        Uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.FirstElement         = 0;
        Uav.Buffer.NumElements          = ObjectCapacity;
        Uav.Buffer.StructureByteStride  = sizeof(u32);
        _SRVHeap.CreateUAV(_Device, ResultBuffer.Get(), Uav, ResultUAVSlot);

        for (u32 s = 0; s < kSlots; ++s)
            TestTableSlot[s] = _SRVHeap.Allocate(2);

        RefreshTestTables(_Device, _SRVHeap);
    }

    // Tabela do teste = 2 descritores contiguos [bounds do slot, cadeia HZB]; depende
    // das duas metades (SetupObjects e SetupForResize), entao e refeita nas duas.
    void FHiZOcclusion::RefreshTestTables(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (!Ready || ObjectCapacity == 0) return;
        for (u32 s = 0; s < kSlots; ++s) {
            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Buffer.FirstElement        = static_cast<u64>(s) * ObjectCapacity;
            Srv.Buffer.NumElements         = ObjectCapacity;
            Srv.Buffer.StructureByteStride = sizeof(FBoundsEntry);
            _SRVHeap.CreateSRV(_Device, BoundsBuffer.Get(), Srv, TestTableSlot[s]);

            D3D12_SHADER_RESOURCE_VIEW_DESC Chain{};
            Chain.Format                    = DXGI_FORMAT_R32_FLOAT;
            Chain.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Chain.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            Chain.Texture2D.MostDetailedMip = 0;
            Chain.Texture2D.MipLevels       = NumMips;
            _SRVHeap.CreateSRV(_Device, HZB.Get(), Chain, TestTableSlot[s] + 1);
        }
    }

    void FHiZOcclusion::RecordBuild(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                                    u32 _DepthSRVSlot) {
        if (!Ready) return;

        {
            FBarrierBatch Batch;
            Batch.Transition(HZB.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Batch.Flush(_Cmd);
        }

        BuildPSO.Bind(_Cmd);

        struct { u32 SrcW, SrcH, DstW, DstH; } C{};
        u32 SrcW = SrcWidth, SrcH = SrcHeight;
        for (u32 m = 0; m < NumMips; ++m) {
            const u32 DstW = std::max(HZBWidth >> m, 1u);
            const u32 DstH = std::max(HZBHeight >> m, 1u);
            C = { SrcW, SrcH, DstW, DstH };
            _Cmd->SetComputeRoot32BitConstants(0, 4, &C, 0);
            _Cmd->SetComputeRootDescriptorTable(
                1, _SRVHeap.GpuHandle(m == 0 ? _DepthSRVSlot : MipSRVSlot[m - 1]));
            _Cmd->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(MipUAVSlot[m]));
            _Cmd->Dispatch((DstW + 7) / 8, (DstH + 7) / 8, 1);

            // O mip recem-escrito vira fonte da proxima reducao (e leitura do teste).
            FBarrierBatch Batch;
            Batch.Transition(HZB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m);
            Batch.Flush(_Cmd);

            SrcW = DstW;
            SrcH = DstH;
        }
    }

    void FHiZOcclusion::WriteBounds(u32 _FrameSlot, u32 _SceneIndex, const Vec3& _AABBMin,
                                    const Vec3& _AABBMax) {
        if (!MappedBounds || _SceneIndex >= ObjectCapacity) return;
        FBoundsEntry E{};
        E.Center[0] = (_AABBMin.X + _AABBMax.X) * 0.5f;
        E.Center[1] = (_AABBMin.Y + _AABBMax.Y) * 0.5f;
        E.Center[2] = (_AABBMin.Z + _AABBMax.Z) * 0.5f;
        E.Extent[0] = (_AABBMax.X - _AABBMin.X) * 0.5f;
        E.Extent[1] = (_AABBMax.Y - _AABBMin.Y) * 0.5f;
        E.Extent[2] = (_AABBMax.Z - _AABBMin.Z) * 0.5f;
        std::memcpy(MappedBounds +
                    (static_cast<size_t>(_FrameSlot) * ObjectCapacity + _SceneIndex) *
                        sizeof(FBoundsEntry),
                    &E, sizeof(E));
    }

    void FHiZOcclusion::RecordTest(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                                   u32 _FrameSlot, u32 _ObjectCount,
                                   const Mat44& _ViewProjNoJitter) {
        FSlotMeta& Meta = SlotMeta[_FrameSlot];
        Meta.Valid = false;
        if (!Ready || ObjectCapacity == 0 || _ObjectCount == 0) return;
        const u32 Count = std::min(_ObjectCount, ObjectCapacity);

        FTestConstants TC{};
        TC.ViewProj    = _ViewProjNoJitter;
        TC.RenderW     = static_cast<f32>(SrcWidth);
        TC.RenderH     = static_cast<f32>(SrcHeight);
        TC.ObjectCount = Count;
        TC.NumMips     = NumMips;
        TC.DepthBias   = 0.0f;
        std::memcpy(MappedTestCB + static_cast<size_t>(_FrameSlot) * 256, &TC, sizeof(TC));

        {
            FBarrierBatch Batch;
            Batch.TransitionTracked(ResultBuffer.Get(), ResultState,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Batch.Flush(_Cmd);
        }

        TestPSO.Bind(_Cmd);
        _Cmd->SetComputeRootConstantBufferView(
            0, TestCB->GetGPUVirtualAddress() + static_cast<u64>(_FrameSlot) * 256);
        _Cmd->SetComputeRootDescriptorTable(1, _SRVHeap.GpuHandle(TestTableSlot[_FrameSlot]));
        _Cmd->SetComputeRootDescriptorTable(2, _SRVHeap.GpuHandle(ResultUAVSlot));
        _Cmd->Dispatch((Count + 63) / 64, 1, 1);

        {
            FBarrierBatch Batch;
            Batch.TransitionTracked(ResultBuffer.Get(), ResultState,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            Batch.Flush(_Cmd);
        }
        _Cmd->CopyBufferRegion(ReadbackBuffer.Get(),
                               static_cast<u64>(_FrameSlot) * ObjectCapacity * sizeof(u32),
                               ResultBuffer.Get(), 0,
                               static_cast<u64>(Count) * sizeof(u32));

        Meta.Count    = Count;
        Meta.RawCount = _ObjectCount;
        Meta.Valid    = true;
    }

    // _ExpectedCount = renderables.size() do frame que consome. Deve bater com a
    // RawCount do teste (mesma cena); a capacidade clampada NAO entra na comparacao
    // (o resultado cobre [0, Count) e o chamador so le indices < Capacity()).
    const u32* FHiZOcclusion::ResolveResults(u32 _FrameSlot, u32 _ExpectedCount) const {
        const FSlotMeta& Meta = SlotMeta[_FrameSlot];
        if (!MappedReadback || !Meta.Valid || Meta.RawCount != _ExpectedCount) return nullptr;
        return MappedReadback + static_cast<size_t>(_FrameSlot) * ObjectCapacity;
    }

    void FHiZOcclusion::InvalidateResults() {
        for (u32 s = 0; s < kSlots; ++s) SlotMeta[s].Valid = false;
    }

    void FHiZOcclusion::CreatePipelines(ID3D12Device* _Device) {
        BuildPSO.Initialize(_Device, "HZBBuild.cs_6_0.cso", false);
        TestPSO.Initialize(_Device, "HZBTest.cs_6_0.cso", 2, 1);
    }


    FPassShaderStems FHiZOcclusion::ShaderStems() const {
        static const char* const kStems[] = { "HZBBuild.cs", "HZBTest.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FHiZOcclusion::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
