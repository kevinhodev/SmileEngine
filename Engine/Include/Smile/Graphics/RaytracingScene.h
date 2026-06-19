#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <unordered_map>

namespace Smile {
    class FD3D12Device;
    class FCommandQueue;
    class FTextureSRVHeap;
    class FScene;
    class FGpuMesh;

    class FRaytracingScene {
    public:
        void Build(FD3D12Device& Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap,
                   const FScene& Scene);

        void Release(FTextureSRVHeap& SRVHeap);

        bool IsBuilt()        const { return Built; }
        u32  TlasSRVSlot()    const { return TlasSRVSlot_; }       
        u32  InstanceCount()  const { return InstanceCount_; }
        u32  BlasCount()      const { return static_cast<u32>(BlasResults.size()); }

    private:
        bool   Built          = false;
        u32    TlasSRVSlot_    = 0xFFFFFFFFu;
        u32    InstanceCount_  = 0;

        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>          BlasResults;
        std::unordered_map<const FGpuMesh*, D3D12_GPU_VIRTUAL_ADDRESS> BlasByMesh;
        Microsoft::WRL::ComPtr<ID3D12Resource>                       Tlas;
    };
}
