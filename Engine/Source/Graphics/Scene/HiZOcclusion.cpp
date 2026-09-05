#include "Smile/Graphics/Scene/HiZOcclusion.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/TextureSRVHeap.h"
#include "Smile/Graphics/Backend/D3D12/Barriers.h"
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
            if (TestTableSlot[s] != kInvalidSlot) { _SRVHeap.Free(TestTableSlot[s], 3); TestTableSlot[s] = kInvalidSlot; }
        }
        if (ResultUAVSlot != kInvalidSlot) { _SRVHeap.Free(ResultUAVSlot); ResultUAVSlot = kInvalidSlot; }
        if (CandidateBuffer && MappedCandidates) {
            CandidateBuffer->Unmap(0, nullptr); MappedCandidates = nullptr;
        }
        if (ReadbackBuffer && MappedReadback) { ReadbackBuffer->Unmap(0, nullptr); MappedReadback = nullptr; }
        if (TestCB && MappedTestCB)           { TestCB->Unmap(0, nullptr);         MappedTestCB = nullptr; }
        CandidateBuffer.Reset();
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

        const u64 CandidateBytes = static_cast<u64>(kSlots) * ObjectCapacity * sizeof(u32);
        const GpuResources::FUploadBuffer Candidates =
            GpuResources::CreateUploadBuffer(_Device, CandidateBytes, 1, false);
        CandidateBuffer = Candidates.Resource;
        MappedCandidates = reinterpret_cast<u32*>(Candidates.Mapped);
        void* P = nullptr;
        std::memset(MappedCandidates, 0, CandidateBytes);

        ResultBuffer = GpuResources::CreateBuffer(
            _Device, static_cast<u64>(ObjectCapacity) * sizeof(u32),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, EVramCategory::Misc,
            "Resultado do culling");
        ResultState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        ReadbackBuffer = GpuResources::CreateReadbackBuffer(
            _Device, static_cast<u64>(kSlots) * ObjectCapacity * sizeof(u32));
        // Map persistente: heap READBACK aceita; a leitura por slot so acontece depois
        // do WaitForValue da fence do slot no BeginFrame (mesmo padrao do GpuProfiler).
        SMILE_HR(ReadbackBuffer->Map(0, nullptr, &P));
        MappedReadback = reinterpret_cast<const u32*>(P);

        const GpuResources::FUploadBuffer Test =
            GpuResources::CreateUploadBuffer(_Device, 256, kSlots);
        TestCB       = Test.Resource;
        MappedTestCB = Test.Mapped;

        ResultUAVSlot = _SRVHeap.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
        Uav.Format                      = DXGI_FORMAT_UNKNOWN;
        Uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
        Uav.Buffer.FirstElement         = 0;
        Uav.Buffer.NumElements          = ObjectCapacity;
        Uav.Buffer.StructureByteStride  = sizeof(u32);
        _SRVHeap.CreateUAV(_Device, ResultBuffer.Get(), Uav, ResultUAVSlot);

        for (u32 s = 0; s < kSlots; ++s)
            TestTableSlot[s] = _SRVHeap.Allocate(3);

        RefreshTestTables(_Device, _SRVHeap);
    }

    void FHiZOcclusion::SetInstanceBounds(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap,
                                          u32 _BoundsSRV, u32 _BoundsCapacity) {
        InstanceBoundsSRV_ = _BoundsSRV;
        InstanceBoundsCapacity_ = _BoundsCapacity;
        RefreshTestTables(_Device, _SRVHeap);
    }

    // Tabela do teste = [slots compactos, bounds canonicos por slot, cadeia HZB]. Depende das
    // tres metades e e refeita sempre que resize, capacidade ou tabela de instancia mudar.
    void FHiZOcclusion::RefreshTestTables(ID3D12Device* _Device, FTextureSRVHeap& _SRVHeap) {
        if (!Ready || ObjectCapacity == 0 || InstanceBoundsSRV_ == kInvalidSlot ||
            InstanceBoundsCapacity_ == 0) return;
        for (u32 s = 0; s < kSlots; ++s) {
            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Buffer.FirstElement        = static_cast<u64>(s) * ObjectCapacity;
            Srv.Buffer.NumElements         = ObjectCapacity;
            Srv.Buffer.StructureByteStride = sizeof(u32);
            _SRVHeap.CreateSRV(_Device, CandidateBuffer.Get(), Srv, TestTableSlot[s]);
            const u32 Sources[2] = { InstanceBoundsSRV_, FullSRVSlot };
            _SRVHeap.CopyTable(_Device, TestTableSlot[s] + 1, Sources);
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

    void FHiZOcclusion::WriteCandidate(u32 _FrameSlot, FGpuSceneHandle Handle) {
        auto& Handles = SlotMeta[_FrameSlot].Handles;
        if (!MappedCandidates || Handles.size() >= ObjectCapacity || Handle.Generation == 0 ||
            Handle.Slot == FGpuSceneHandle::kInvalidSlot ||
            Handle.Slot >= InstanceBoundsCapacity_) return;
        const u32 SceneIndex = static_cast<u32>(Handles.size());
        Handles.push_back(Handle);
        MappedCandidates[static_cast<size_t>(_FrameSlot) * ObjectCapacity + SceneIndex] =
            Handle.Slot;
    }

    void FHiZOcclusion::RecordTest(ID3D12GraphicsCommandList* _Cmd, FTextureSRVHeap& _SRVHeap,
                                   u32 _FrameSlot, const Mat44& _ViewProjNoJitter) {
        FSlotMeta& Meta = SlotMeta[_FrameSlot];
        Meta.Valid = false;
        const u32 Count = static_cast<u32>(Meta.Handles.size());
        if (!Ready || ObjectCapacity == 0 || Count == 0) return;

        FTestConstants TC{};
        TC.ViewProj    = _ViewProjNoJitter;
        TC.RenderW     = static_cast<f32>(SrcWidth);
        TC.RenderH     = static_cast<f32>(SrcHeight);
        TC.ObjectCount = Count;
        TC.NumMips     = NumMips;
        TC.DepthBias   = 0.0f;
        TC.InstanceCapacity = InstanceBoundsCapacity_;
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
        Meta.Valid    = true;
    }

    void FHiZOcclusion::BeginFrame(u32 _FrameSlot, const FGpuSceneSlots& Slots) {
        FSlotMeta& Meta = SlotMeta[_FrameSlot];
        if (MappedReadback && Meta.Valid && Meta.Count == Meta.Handles.size()) {
            const u32* Results = MappedReadback + static_cast<size_t>(_FrameSlot) * ObjectCapacity;
            Visibility.Publish(Slots, Meta.Handles, std::span<const u32>(Results, Meta.Count));
        } else {
            Visibility.Publish(Slots, {}, {});
        }
        Meta.Valid = false;
        Meta.Count = 0;
        Meta.Handles.clear();
    }

    void FHiZOcclusion::InvalidateResults() {
        for (u32 s = 0; s < kSlots; ++s) SlotMeta[s].Valid = false;
        Visibility.Invalidate();
    }

    void FHiZOcclusion::CreatePipelines(ID3D12Device* _Device) {
        BuildPSO.Initialize(_Device, "HZBBuild.cs_6_0.cso", false);
        TestPSO.Initialize(_Device, "HZBTest.cs_6_0.cso", 3, 1);
    }


    FPassShaderStems FHiZOcclusion::ShaderStems() const {
        static const char* const kStems[] = { "HZBBuild.cs", "HZBTest.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FHiZOcclusion::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
