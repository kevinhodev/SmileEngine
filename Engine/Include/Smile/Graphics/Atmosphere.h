#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/VolumeTexture.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FCommandQueue;

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

        Mat44 InvViewProj;       // FULL inverse view-proj (with translation) — froxel world reconstruction
        Vec4 CameraWorldPos;     // xyz = camera world position (scene units), w = km per world unit
        Vec4 AerialParams;       // x = volume depth (km), y = slice count, z = start depth (km), w = samples/slice

        Vec4 MoonDir;            // xyz = direction TO moon (world), w = cos(raio angular do disco)
        Vec4 MoonParams;         // x = brilho do disco, y = intensidade estrelas, z = night factor, w = tempo (cintilacao)
        Vec4 StarAxis;           // xyz = polo celeste (mundo), w = angulo da rotacao diurna (rad)
    };

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

    class FAtmosphere {
    public:
        static constexpr u32 kTransmittanceW = 256;
        static constexpr u32 kTransmittanceH = 64;
        static constexpr u32 kMultiScatterW  = 32;
        static constexpr u32 kMultiScatterH  = 32;
        static constexpr u32 kSkyViewW       = 192;
        static constexpr u32 kSkyViewH       = 104;
 
        static constexpr u32 kAerialW        = 32;
        static constexpr u32 kAerialH        = 32;
        static constexpr u32 kAerialSlices   = 16;

        static constexpr f32 kGroundAltitudeKm = 0.5f;

        void Initialize(ID3D12Device* Device, FCommandQueue& CmdQueue,
                        FTextureSRVHeap& SRVHeap,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        void RecreateSky(ID3D12Device* Device,
                         DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        void UpdatePerFrame(u32 FrameSlot, const Vec3& DirToSun, const Mat44& InvViewProjNoTranslation,
                            const Mat44& InvViewProjFull, const Vec3& CameraWorldPos, f32 KmPerWorldUnit);

        void SetNightParams(const Vec3& DirToMoon, f32 CosDiskRadius, f32 DiskBrightness,
                            f32 StarIntensity, f32 NightFactor, f32 TimeSec);
        void SetStarRotation(const Vec3& PoleAxis, f32 AngleRad);

        void SetSunDiskHalfAngle(f32 DegHalfAngle);
        void SetSunGlare(f32 Intensity);
        f32  GetSunDiskHalfAngle() const;
        f32  GetSunGlare() const;

        void RecordSkyViewBake(ID3D12GraphicsCommandList* CommandList);
        void RecordAerialPerspectiveBake(ID3D12GraphicsCommandList* CommandList);

        void RenderSky(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        void BakeIfDirty(ID3D12Device* Device, FCommandQueue& CmdQueue);
        void MarkDirty() { Dirty = true; }

        u32 TransmittanceSRV() const { return Transmittance.SRVSlot; }
        u32 MultiScatterSRV()  const { return MultiScatter.SRVSlot; }
        u32 SkyViewSRV()       const { return SkyView.SRVSlot; }
        u32 AerialVolumeSRV()  const { return AerialPerspectiveVolume.SRVSlot(); }
        f32 AerialDepthKm()    const { return CPUConstants.AerialParams.X; }
        D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress() const { return CBAddr(); }
        bool IsInitialized() const { return Initialized; }

        void LoadMoonTexture(ID3D12Device* Device, FCommandQueue& CmdQueue,
                             FTextureSRVHeap& SRVHeap, const std::wstring& Path);
        bool HasMoonTexture() const { return MoonTexLoaded; }

        Vec3 SunTransmittance(const Vec3& DirToSun) const;

    private:
        static constexpr D3D12_RESOURCE_STATES kReadState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        void CreateConstantBuffer(ID3D12Device* Device);
        void BuildInputTables(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildSkyRootSignature(ID3D12Device* Device);
        void BuildSkyPSO(ID3D12Device* Device,
                         DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Bake(ID3D12Device* Device, FCommandQueue& CmdQueue);

        FTextureSRVHeap*    SRVHeapPtr = nullptr;
        FLut2D              Transmittance;
        FLut2D              MultiScatter;
        FLut2D              SkyView;

        FTexture            MoonTexture;
        bool                MoonTexLoaded = false;
        FVolumetricPipeline TransmittancePSO;
        FVolumetricPipeline MultiScatterPSO;
        FVolumetricPipeline SkyViewPSO;

        FVolumeTexture      AerialPerspectiveVolume;
        FVolumetricPipeline AerialPerspectivePSO;

        u32 SkyViewBakeTableStart = 0; 
        u32 SkyRenderTableStart   = 0; 

        Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPSO;

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
