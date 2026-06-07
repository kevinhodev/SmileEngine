#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FCommandQueue;

    // Physical-atmosphere parameters + LUT sizes. Must match the AtmosphereCB
    // cbuffer in Shaders/Atmosphere/AtmosphereCommon.hlsli field-for-field.
    // Distances in kilometers; scattering/extinction in km^-1.
    struct alignas(256) AtmosphereConstants {
        Vec4 RayleighScattering; // rgb km^-1, w = Rayleigh density scale height (km)
        Vec4 MieScattering;      // rgb km^-1, w = Mie density scale height (km)
        Vec4 MieExtinction;      // rgb km^-1, w = Mie phase anisotropy g
        Vec4 OzoneAbsorption;    // rgb km^-1, w = unused
        Vec4 OzoneTent;          // x = center altitude (km), y = half-width (km)
        Vec4 GroundAlbedo;       // rgb, w = unused
        Vec4 PlanetRadii;        // x = bottom (planet) km, y = top (atmosphere) km
        Vec4 SunDir;             // xyz = direction TO sun, w = sun illuminance
        Vec4 AtmoSteps;          // x = transmittance, y = multiscatter, z = sky-view steps
        Vec4 LutSize;            // x = transW, y = transH, z = multiW, w = multiH
        Vec4 SkyViewSize;        // x = skyW, y = skyH, z = camera view height (km), w = ground altitude (km)
        Vec4 SunDisk;            // x = cos(half angle), y = disk intensity, z = sun illuminance (sky-view)
        Mat44 InvViewProjNoTrans; // sky PS world-ray reconstruction
    };

    // A 2D compute-written lookup texture (resource + SRV + UAV + tracked state).
    struct FLut2D {
        void Create(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                    DXGI_FORMAT Format, u32 Width, u32 Height);
        void Transition(ID3D12GraphicsCommandList* CommandList,
                        D3D12_RESOURCE_STATES After);

        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        u32 SRVSlot = 0;
        u32 UAVSlot = 0;
        u32 W = 0, H = 0;
        DXGI_FORMAT Fmt = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };

    // Hillaire "Scalable and Production Ready Sky and Atmosphere": owns the
    // Transmittance and Multi-Scattering LUTs (A1). The Sky-View LUT, aerial
    // perspective and sky rendering are added in later stages.
    class FAtmosphere {
    public:
        static constexpr u32 kTransmittanceW = 256;
        static constexpr u32 kTransmittanceH = 64;
        static constexpr u32 kMultiScatterW  = 32;
        static constexpr u32 kMultiScatterH  = 32;
        static constexpr u32 kSkyViewW       = 192;
        static constexpr u32 kSkyViewH       = 104;
        // Camera altitude above the planet surface (km). The atmosphere lives in
        // its own km-scaled frame, decoupled from the scene's engine units.
        static constexpr f32 kGroundAltitudeKm = 0.5f;

        // Allocates LUTs + compute pipelines + the sky graphics pipeline + the
        // constant buffer, sets default Earth-like parameters, builds the
        // contiguous descriptor tables, and bakes the transmittance/multi-scatter
        // LUTs once. SampleCount/RT/DS format the sky pass graphics PSO.
        void Initialize(ID3D12Device* Device, FCommandQueue& CmdQueue,
                        FTextureSRVHeap& SRVHeap, u32 SampleCount,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        // Rebuild the sky graphics PSO (MSAA change).
        void RecreateSky(ID3D12Device* Device, u32 SampleCount,
                         DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        // Per-frame: update sun direction + camera-relative view matrix in the CB.
        void UpdatePerFrame(u32 FrameSlot, const Vec3& DirToSun, const Mat44& InvViewProjNoTranslation);

        // Sun disk controls (CB-only, no LUT re-bake).
        void SetSunDiskHalfAngle(f32 DegHalfAngle);
        void SetSunGlare(f32 Intensity);
        f32  GetSunDiskHalfAngle() const;
        f32  GetSunGlare() const;

        // Records the per-frame sky-view LUT bake (compute) into the given list.
        // Caller must have already set descriptor heaps. Leaves the sky-view LUT
        // in a shader-readable state for RenderSky.
        void RecordSkyViewBake(ID3D12GraphicsCommandList* CommandList);

        // Records the fullscreen sky draw (graphics). Caller must have set the
        // render/depth targets, viewport, scissor and descriptor heaps.
        void RenderSky(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        // Re-bakes the transmittance + multi-scatter LUTs if parameters changed.
        void BakeIfDirty(ID3D12Device* Device, FCommandQueue& CmdQueue);
        void MarkDirty() { Dirty = true; }

        u32 TransmittanceSRV() const { return Transmittance.SRVSlot; }
        u32 MultiScatterSRV()  const { return MultiScatter.SRVSlot; }
        u32 SkyViewSRV()       const { return SkyView.SRVSlot; }
        D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress() const { return CBAddr(); }
        bool IsInitialized() const { return Initialized; }

    private:
        // States the LUTs rest in between passes (read by both compute and pixel).
        static constexpr D3D12_RESOURCE_STATES kReadState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        void CreateConstantBuffer(ID3D12Device* Device);
        void BuildInputTables(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildSkyRootSignature(ID3D12Device* Device);
        void BuildSkyPSO(ID3D12Device* Device, u32 SampleCount,
                         DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Bake(ID3D12Device* Device, FCommandQueue& CmdQueue);

        FTextureSRVHeap*    SRVHeapPtr = nullptr;
        FLut2D              Transmittance;
        FLut2D              MultiScatter;
        FLut2D              SkyView;
        FVolumetricPipeline TransmittancePSO;
        FVolumetricPipeline MultiScatterPSO;
        FVolumetricPipeline SkyViewPSO;

        // Contiguous 2-slot SRV tables built once via CopyDescriptors.
        u32 SkyViewBakeTableStart = 0; // [transmittance(t0), multiscatter(t1)]
        u32 SkyRenderTableStart   = 0; // [skyview(t0),       transmittance(t1)]

        // Sky graphics pipeline (fullscreen pass).
        Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPSO;

        // CB double-buffered: array de kFramesInFlight copias de AtmosphereConstants.
        // CPUConstants eh o shadow; cada frame copia o shadow inteiro p/ sua regiao.
        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8*                  MappedBase = nullptr;
        u32                  FrameSlot  = 0;
        AtmosphereConstants  CPUConstants{};

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(AtmosphereConstants);
        }
        AtmosphereConstants* Mapped() const {
            return reinterpret_cast<AtmosphereConstants*>(
                MappedBase + static_cast<size_t>(FrameSlot) * sizeof(AtmosphereConstants));
        }

        bool Dirty       = true;
        bool Initialized = false;
    };
}
