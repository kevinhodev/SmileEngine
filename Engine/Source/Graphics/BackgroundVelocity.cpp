#include "Smile/Graphics/BackgroundVelocity.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>

namespace Smile {
    namespace {
        // CBV precisa de 256B por slot; a matriz sozinha e 64B.
        constexpr u32 kCBStride = 256;
    }

    void FBackgroundVelocity::Initialize(ID3D12Device* Device) {
        // Mesma layout de root sig do RRGuides: CBV b0 + tabela SRV (1) + tabela UAV (1).
        PSO.Initialize(Device, "BackgroundVelocity.cs_6_0.cso", 1, 1, false);

        D3D12_HEAP_PROPERTIES Heap{}; Heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width            = static_cast<u64>(FCommandQueue::kFramesInFlight) * kCBStride;
        Desc.Height           = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels        = 1;
        Desc.Format           = DXGI_FORMAT_UNKNOWN;
        Desc.SampleDesc       = { 1, 0 };
        Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        SMILE_HR(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Constants)));
        D3D12_RANGE NoRead{ 0, 0 };
        void* Ptr = nullptr;
        SMILE_HR(Constants->Map(0, &NoRead, &Ptr));
        MappedConstants = static_cast<u8*>(Ptr);

        Initialized = true;
    }

    void FBackgroundVelocity::Shutdown() {
        if (Constants && MappedConstants) { Constants->Unmap(0, nullptr); MappedConstants = nullptr; }
        Constants.Reset();
        Initialized = false;
    }

    void FBackgroundVelocity::Record(ID3D12GraphicsCommandList* CL, u32 FrameSlot,
                                     D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv,
                                     D3D12_GPU_DESCRIPTOR_HANDLE VelocityUav,
                                     const Mat44& SkyClipToPrevClip, u32 Width, u32 Height) {
        if (!Initialized || Width == 0 || Height == 0) return;

        std::memcpy(MappedConstants + static_cast<u64>(FrameSlot) * kCBStride,
                    &SkyClipToPrevClip, sizeof(Mat44));

        PSO.Bind(CL);
        CL->SetComputeRootConstantBufferView(
            0, Constants->GetGPUVirtualAddress() + static_cast<u64>(FrameSlot) * kCBStride);
        CL->SetComputeRootDescriptorTable(1, DepthSrv);
        CL->SetComputeRootDescriptorTable(2, VelocityUav);
        const u32 GX = (Width + 7) / 8, GY = (Height + 7) / 8;
        CL->Dispatch(GX, GY, 1);
    }
}
