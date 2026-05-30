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

    // Owns the IBL cubemap chain (env, irradiance, prefiltered specular) and the
    // 2D BRDF LUT. Generates everything on the GPU from a loaded HDR (.hdr) so the
    // user can swap environments at runtime from the editor.
    class FHDREnvironment {
    public:
        // Compile-time configuration. Powers of two; mip counts match.
        static constexpr u32 kEnvCubeSize        = 1024;
        static constexpr u32 kEnvCubeMips        = 11; // log2(1024) + 1 — full chain for Karis solid-angle correction
        static constexpr u32 kIrradianceSize     = 32;
        static constexpr u32 kSpecularSize       = 128;
        static constexpr u32 kSpecularMips       = 7;  // mip 0..6 → roughness 0..1
        static constexpr u32 kBRDFLutSize        = 128;
        static constexpr u32 kSpecularSampleCount = 512;

        // One-time setup. Allocates the cubemap resources at the configured sizes
        // (kept across HDR swaps), creates the four compute PSOs, generates the
        // BRDF LUT, and uploads a neutral default 1x1 black so the PS has valid
        // SRVs even before any HDR is loaded.
        void Initialize(ID3D12Device* Device, FCommandQueue& CmdQueue,
                        FTextureSRVHeap& SRVHeap);

        // Loads an .hdr from disk (Radiance RGBE via stb_image), uploads it as a
        // 2D R32G32B32A32_FLOAT texture, then runs the four compute passes.
        // Returns false on file/IO failure (logs the reason).
        bool LoadFromFile(ID3D12Device* Device, FCommandQueue& CmdQueue,
                          FTextureSRVHeap& SRVHeap, const std::wstring& Path);

        u32  EnvCubeSRV()     const { return EnvCube.SRVSlot(); }
        u32  IrradianceSRV()  const { return IrradianceCube.SRVSlot(); }
        u32  SpecularSRV()    const { return SpecularCube.SRVSlot(); }
        u32  BRDFLutSRV()     const { return BRDFLutSRVSlot; }
        bool HasHDRLoaded()   const { return HDRLoaded; }

    private:
        // Uploads stb-decoded float RGBA to a 2D HDR texture and returns the SRV slot.
        u32 UploadEquirect2D(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap,
                             const float* Pixels, u32 Width, u32 Height);

        // Runs the full IBL chain off the current Equirect2DResource.
        void RunIBLChain(ID3D12Device* Device, FCommandQueue& CmdQueue,
                         FTextureSRVHeap& SRVHeap, u32 EquirectSRVSlot);

        // Generates the BRDF LUT once and stores it in BRDFLutResource/Slot.
        void GenerateBRDFLut(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap);

        FCubeTexture EnvCube;          // 512^2, 10 mips, UAV (write from equirect CS + mip-gen)
        FCubeTexture IrradianceCube;   // 32^2, 1 mip, UAV
        FCubeTexture SpecularCube;     // 128^2, 7 mips, UAV (one per mip)

        // BRDF LUT is a plain 2D R16G16_FLOAT, independent of the cube wrapper.
        Microsoft::WRL::ComPtr<ID3D12Resource> BRDFLutResource;
        u32 BRDFLutSRVSlot = 0;
        u32 BRDFLutUAVSlot = 0;

        // Equirect upload resource (re-created per LoadFromFile; kept alive so
        // the SRV slot stays valid until next swap).
        Microsoft::WRL::ComPtr<ID3D12Resource> Equirect2DResource;
        u32 Equirect2DSRVSlot = 0;

        // Compute pipelines (created in Initialize).
        FComputePipeline EquirectToCubePSO;
        FComputePipeline MipGenPSO;
        FComputePipeline IrradiancePSO;
        FComputePipeline SpecularPSO;
        FComputePipeline BRDFLutPSO;

        bool HDRLoaded = false;
    };
}
