#include "Smile/Graphics/BackgroundVelocity.h"
#include "Smile/Graphics/GpuResources.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>
#include <iterator>

namespace Smile {
    namespace {
        // CBV precisa de 256B por slot; a matriz sozinha e 64B.
        constexpr u32 kCBStride = 256;
    }

    void FBackgroundVelocity::Initialize(ID3D12Device* Device) {
        // Mesma layout de root sig do RRGuides: CBV b0 + tabela SRV (1) + tabela UAV (1).
        CreatePipelines(Device);

        const GpuResources::FUploadBuffer Upload = GpuResources::CreateUploadBuffer(
            Device, kCBStride, FCommandQueue::kFramesInFlight);
        Constants       = Upload.Resource;
        MappedConstants = Upload.Mapped;

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

    void FBackgroundVelocity::CreatePipelines(ID3D12Device* _Device) {
        PSO.Initialize(_Device, "BackgroundVelocity.cs_6_0.cso", 1, 1, false);
    }


    FPassShaderStems FBackgroundVelocity::ShaderStems() const {
        static const char* const kStems[] = { "BackgroundVelocity.cs" };
        return { kStems, static_cast<u32>(std::size(kStems)) };
    }

    void FBackgroundVelocity::OnRecreatePipelines(const FPassInitContext& _Ctx) {
        CreatePipelines(_Ctx.Device);
    }

}
