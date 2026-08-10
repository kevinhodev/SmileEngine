#include "Smile/Graphics/ReGIR.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Core/HResultCheck.h"
#include <algorithm>
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace Smile {
    namespace {
        constexpr u32 kCellCount = FReGIR::kGridX * FReGIR::kGridY * FReGIR::kGridZ;
        constexpr u32 kSlotCount = kCellCount * FReGIR::kSlotsPerCell;

        // ReGIRAverage.cs.hlsl fixa numthreads(64,1,1), groupshared[64] e stride inicial 32 da
        // reducao. Com mais de 64 slots ele promediaria SO os 64 primeiros e mesmo assim dividiria
        // por slotsPerCell — e esse average e o denominador do sourcePdf da etapa 2, entao o erro
        // sai como BRILHO errado em toda a luz local indireta, sem nada quebrar visivelmente.
        static_assert(FReGIR::kSlotsPerCell == 64,
                      "Ao mudar kSlotsPerCell, ajuste ReGIRAverage.cs.hlsl: numthreads, "
                      "SharedWeight[] e o stride da reducao (ou faca o load em passos de 64).");

        ComPtr<ID3D12Resource> CreateStructuredUAV(ID3D12Device* Device, u32 Count, u32 Stride) {
            return GpuResources::CreateBuffer(
                Device, static_cast<u64>(Count) * Stride,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
                EVramCategory::GI, "ReGIR · grade");
        }
    }

    void FReGIR::Initialize(ID3D12Device* Device) {
        BuildPSO.Initialize(Device, "ReGIRBuild.cs_6_6.cso", 2, 1, false);
        AveragePSO.Initialize(Device, "ReGIRAverage.cs_6_0.cso", 1, 1, false);
        CreateConstantBuffer(Device);
        Initialized = true;
    }

    void FReGIR::RecreatePSO(ID3D12Device* Device) {
        if (!Initialized) return;
        BuildPSO.Initialize(Device, "ReGIRBuild.cs_6_6.cso", 2, 1, false);
        AveragePSO.Initialize(Device, "ReGIRAverage.cs_6_0.cso", 1, 1, false);
    }

    void FReGIR::CreateConstantBuffer(ID3D12Device* Device) {
        D3D12_HEAP_PROPERTIES Heap{};
        Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<UINT64>(FCommandQueue::kFramesInFlight) *
                                sizeof(ReGIRConstants);
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
    }

    D3D12_GPU_VIRTUAL_ADDRESS FReGIR::CBAddr() const {
        return CB->GetGPUVirtualAddress() +
               static_cast<UINT64>(FrameSlot) * sizeof(ReGIRConstants);
    }

    void FReGIR::ReleaseScene(FTextureSRVHeap& SRVHeap) {
        auto Free = [&](u32& Slot, u32 Count) {
            if (Slot != kInvalidSlot) { SRVHeap.Free(Slot, Count); Slot = kInvalidSlot; }
        };
        for (u32 p = 0; p < 2; ++p) {
            Free(SlotsSRV[p], 1); Free(SlotsUAV[p], 1);
            Free(AverageSRV[p], 1); Free(AverageUAV[p], 1);
            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f)
                Free(BuildTable[p][f], 2);
            Slots[p].Reset(); CellAverage[p].Reset();
            SlotsState[p] = AverageState[p] = D3D12_RESOURCE_STATE_COMMON;
        }
        WriteParity = CurrentParity = 0;
        HistoryValid = HasCurrent = Ready = false;
        LastLightSetSignature = 0;
    }

    void FReGIR::SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                               const Vec3& AABBMin, const Vec3& AABBMax,
                               const u32 LightSlots[FCommandQueue::kFramesInFlight]) {
        if (!Initialized) return;
        ReleaseScene(SRVHeap);
        for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f)
            if (LightSlots[f] == kInvalidSlot) return;

        // Pequena guarda ao redor da AABB: o jitter estocastico de lookup nao deve perder a
        // grade por erro numerico numa superficie exatamente sobre o limite da cena.
        const f32 Ex = std::max(AABBMax.X - AABBMin.X, 0.1f);
        const f32 Ey = std::max(AABBMax.Y - AABBMin.Y, 0.1f);
        const f32 Ez = std::max(AABBMax.Z - AABBMin.Z, 0.1f);
        const f32 Px = Ex * 0.01f + 0.05f;
        const f32 Py = Ey * 0.01f + 0.05f;
        const f32 Pz = Ez * 0.01f + 0.05f;
        GridMin = { AABBMin.X - Px, AABBMin.Y - Py, AABBMin.Z - Pz };
        CellSize = { (Ex + 2.0f * Px) / static_cast<f32>(kGridX),
                     (Ey + 2.0f * Py) / static_cast<f32>(kGridY),
                     (Ez + 2.0f * Pz) / static_cast<f32>(kGridZ) };

        for (u32 p = 0; p < 2; ++p) {
            Slots[p]       = CreateStructuredUAV(Device, kSlotCount, sizeof(FReGIRReservoirGPU));
            CellAverage[p] = CreateStructuredUAV(Device, kCellCount, sizeof(f32));

            D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
            Srv.Format                     = DXGI_FORMAT_UNKNOWN;
            Srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            Srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            Srv.Buffer.NumElements         = kSlotCount;
            Srv.Buffer.StructureByteStride = sizeof(FReGIRReservoirGPU);
            D3D12_UNORDERED_ACCESS_VIEW_DESC Uav{};
            Uav.Format                     = DXGI_FORMAT_UNKNOWN;
            Uav.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
            Uav.Buffer.NumElements         = kSlotCount;
            Uav.Buffer.StructureByteStride = sizeof(FReGIRReservoirGPU);
            SlotsSRV[p] = SRVHeap.Allocate(1); SlotsUAV[p] = SRVHeap.Allocate(1);
            SRVHeap.CreateSRV(Device, Slots[p].Get(), Srv, SlotsSRV[p]);
            SRVHeap.CreateUAV(Device, Slots[p].Get(), Uav, SlotsUAV[p]);

            Srv.Buffer.NumElements = kCellCount; Srv.Buffer.StructureByteStride = sizeof(f32);
            Uav.Buffer.NumElements = kCellCount; Uav.Buffer.StructureByteStride = sizeof(f32);
            AverageSRV[p] = SRVHeap.Allocate(1); AverageUAV[p] = SRVHeap.Allocate(1);
            SRVHeap.CreateSRV(Device, CellAverage[p].Get(), Srv, AverageSRV[p]);
            SRVHeap.CreateUAV(Device, CellAverage[p].Get(), Uav, AverageUAV[p]);
        }

        // Para escrever p, o historico esta em 1-p. A segunda entrada e a fatia de luz do frame.
        for (u32 p = 0; p < 2; ++p) {
            const u32 Prev = 1u - p;
            for (u32 f = 0; f < FCommandQueue::kFramesInFlight; ++f) {
                BuildTable[p][f] = SRVHeap.Allocate(2);
                D3D12_CPU_DESCRIPTOR_HANDLE Dst = SRVHeap.CpuHandle(BuildTable[p][f]);
                D3D12_CPU_DESCRIPTOR_HANDLE Src[2] = {
                    SRVHeap.CpuHandleStaging(SlotsSRV[Prev]),
                    SRVHeap.CpuHandleStaging(LightSlots[f]),
                };
                UINT Two = 2; UINT Ones[2] = { 1, 1 };
                Device->CopyDescriptors(1, &Dst, &Two, 2, Src, Ones,
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
        Ready = true;
    }

    void FReGIR::UpdatePerFrame(u32 InFrameSlot, u32 FrameIndex, u32 LightCount,
                                u64 LightSetSignature) {
        FrameSlot = InFrameSlot;
        if (!Ready || !MappedCB) return;
        if (LightSetSignature != LastLightSetSignature) {
            HistoryValid = false;
            LastLightSetSignature = LightSetSignature;
        }
        CPU.GridMinSlots    = { GridMin.X, GridMin.Y, GridMin.Z,
                                static_cast<f32>(kSlotsPerCell) };
        CPU.CellSizeHistory = { CellSize.X, CellSize.Y, CellSize.Z,
                                static_cast<f32>(kMaxHistory) };
        CPU.GridCountLights = { static_cast<f32>(kGridX), static_cast<f32>(kGridY),
                                static_cast<f32>(kGridZ), static_cast<f32>(LightCount) };
        CPU.BuildParams     = { static_cast<f32>(FrameIndex), HistoryValid ? 1.0f : 0.0f,
                                static_cast<f32>(kInitialCandidates), 0.0f };
        std::memcpy(MappedCB + static_cast<size_t>(FrameSlot) * sizeof(ReGIRConstants),
                    &CPU, sizeof(CPU));
    }

    void FReGIR::Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
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

    void FReGIR::RecordBuild(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap) {
        if (!Ready) return;
        const u32 Curr = WriteParity;
        const u32 Prev = 1u - Curr;

        Transition(CL, Slots[Prev].Get(), SlotsState[Prev],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(CL, Slots[Curr].Get(), SlotsState[Curr],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(CL, CellAverage[Curr].Get(), AverageState[Curr],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        BuildPSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(BuildTable[Curr][FrameSlot]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(SlotsUAV[Curr]));
        CL->Dispatch((kSlotCount + 63u) / 64u, 1, 1);

        // A transicao ordena as escritas UAV do build antes da reducao por celula.
        Transition(CL, Slots[Curr].Get(), SlotsState[Curr],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        AveragePSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(0, CBAddr());
        CL->SetComputeRootDescriptorTable(1, SRVHeap.GpuHandle(SlotsSRV[Curr]));
        CL->SetComputeRootDescriptorTable(2, SRVHeap.GpuHandle(AverageUAV[Curr]));
        CL->Dispatch(kCellCount, 1, 1);
        Transition(CL, CellAverage[Curr].Get(), AverageState[Curr],
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        CurrentParity = Curr;
        WriteParity = Prev;
        HasCurrent = true;
        HistoryValid = true;
    }

    FReGIRShaderParams FReGIR::ShaderParams(bool Enabled) const {
        const bool On = Enabled && HasOutput();
        FReGIRShaderParams P{};
        P.GridMinSlots       = { GridMin.X, GridMin.Y, GridMin.Z,
                                 static_cast<f32>(kSlotsPerCell) };
        P.InvCellSizeEnabled = { 1.0f / CellSize.X, 1.0f / CellSize.Y, 1.0f / CellSize.Z,
                                 On ? 1.0f : 0.0f };
        P.GridCountSamples   = { static_cast<f32>(kGridX), static_cast<f32>(kGridY),
                                 static_cast<f32>(kGridZ), static_cast<f32>(kShadingCandidates) };
        P.Resources          = { static_cast<f32>(On ? SlotsSRV[CurrentParity] : 0u),
                                 static_cast<f32>(On ? AverageSRV[CurrentParity] : 0u), 0.0f, 0.0f };
        return P;
    }

    FPassShaderStems FReGIR::ShaderStems() const {
        static const char* const kStems[] = { "ReGIRBuild.cs", "ReGIRAverage.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FReGIR::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        if (Ready) RecreatePSO(_Ctx.Device);
    }

}
