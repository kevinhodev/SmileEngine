#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/CubeTexture.h"
#include "Smile/Graphics/ComputePipeline.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace Smile {
    class FCommandQueue;

    class FHDREnvironment {
    public:
        static constexpr u32 kEnvCubeSize        = 1024;
        static constexpr u32 kEnvCubeMips        = 11; 
        static constexpr u32 kIrradianceSize     = 32;
        static constexpr u32 kSpecularSize       = 128;
        static constexpr u32 kSpecularMips       = 7;  
        static constexpr u32 kBRDFLutSize        = 128;
        static constexpr u32 kSpecularSampleCount = 512;

        void Initialize(ID3D12Device* Device, FCommandQueue& CmdQueue,
                        FTextureSRVHeap& SRVHeap);

        bool LoadFromFile(ID3D12Device* Device, FCommandQueue& CmdQueue,
                          FTextureSRVHeap& SRVHeap, const std::wstring& Path);

        u32  EnvCubeSRV()     const { return EnvCube.SRVSlot(); }
        u32  IrradianceSRV()  const { return IrradianceCube.SRVSlot(); }
        u32  SpecularSRV()    const { return SpecularCube.SRVSlot(); }
        u32  BRDFLutSRV()     const { return BRDFLutSRVSlot; }
        bool HasHDRLoaded()   const { return HDRLoaded; }

    private:
        u32 UploadEquirect2D(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap,
                             const float* Pixels, u32 Width, u32 Height);

        void RunIBLChain(ID3D12Device* Device, FCommandQueue& CmdQueue,
                         FTextureSRVHeap& SRVHeap, u32 EquirectSRVSlot);

        void GenerateBRDFLut(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap);

        FCubeTexture EnvCube;          // 512^2, 10 mips, UAV (write from equirect CS + mip-gen)
        FCubeTexture IrradianceCube;   // 32^2, 1 mip, UAV
        FCubeTexture SpecularCube;     // 128^2, 7 mips, UAV (one per mip)

        Microsoft::WRL::ComPtr<ID3D12Resource> BRDFLutResource;
        u32 BRDFLutSRVSlot = 0;
        u32 BRDFLutUAVSlot = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> Equirect2DResource;
        u32 Equirect2DSRVSlot = 0;

        FComputePipeline EquirectToCubePSO;
        FComputePipeline MipGenPSO;
        FComputePipeline IrradiancePSO;
        FComputePipeline SpecularPSO;
        FComputePipeline BRDFLutPSO;

        bool HDRLoaded = false;
    };
}
