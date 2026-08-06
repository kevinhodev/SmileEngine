#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include "Smile/Graphics/Texture.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/VolumeTexture.h"
#include "Smile/Graphics/CubeTexture.h"
#include "Smile/Graphics/ComputePipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FCommandQueue;
    class FUploadQueue;

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
        Vec4 SkyViewSize;        // x=skyW, y=skyH, z=camera radius (km), w=planet radius offset (km)
        Vec4 SunDisk;            // x = cos(half angle), y = disk intensity, z = sun illuminance (sky-view)
        Mat44 InvViewProjNoTrans; // sky PS world-ray reconstruction

        Mat44 InvViewProj;       // FULL inverse view-proj (with translation) — froxel world reconstruction
        Vec4 CameraWorldPos;     // xyz = camera world position (scene units), w = km per world unit
        Vec4 AerialParams;       // x = volume depth (km), y = slice count, z = start depth (km), w = samples/slice

        Vec4 MoonDir;            // xyz = direction TO moon (world), w = cos(raio angular do disco)
        Vec4 MoonParams;         // x = brilho do disco, y = intensidade estrelas, z = night factor, w = tempo (cintilacao)
        Vec4 StarAxis;           // xyz = polo celeste (mundo), w = angulo sideral da matriz (rad)
        Vec4 NightSky;           // x = iluminancia lua, y = corona, z = catalogo ativo, w = livre

        Mat44 ViewProjNoTrans;   // view-proj SEM translacao — projeta o quad das estrelas
        Mat44 StarMatrix;        // frame do catalogo (polo=+Y) -> mundo, com rotacao sideral
        Vec4  StarView;          // xy = output W/H, zw = escala render/output X/Y

        // Dimensoes do volume de aerial perspective. Estavam hardcoded como 32x32 dentro do
        // BakeAerialPerspective.cs.hlsl enquanto o C++ ja tinha kAerialW/kAerialH: mudar as
        // constantes do lado do C++ nao chegava no shader (o bake escreveria so um canto do
        // volume, ou pior, sairia fora). A contagem de slices ja vive no AerialParams.y.
        Vec4  AerialVolumeSize;  // x = W, y = H, zw = livres
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

        // UE/Hillaire-style numerical offset: keep the camera just outside the
        // planet without pretending sea level is 500 m above the ground sphere.
        static constexpr f32 kPlanetRadiusOffsetKm = 0.001f;

        void Initialize(ID3D12Device* Device, FCommandQueue& CmdQueue,
                        FUploadQueue& UploadQueue, FTextureSRVHeap& SRVHeap,
                        DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        void RecreateSky(ID3D12Device* Device,
                         DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);

        void UpdatePerFrame(u32 FrameSlot, const Vec3& DirToSun, const Mat44& InvViewProjNoTranslation,
                            const Mat44& ViewProjNoTranslation, const Mat44& InvViewProjFull,
                            const Vec3& CameraWorldPos, f32 KmPerWorldUnit,
                            f32 RenderW, f32 RenderH,
                            f32 OutputW, f32 OutputH);

        void SetNightParams(const Vec3& DirToMoon, f32 CosDiskRadius, f32 DiskBrightness,
                            f32 StarIntensity, f32 NightFactor, f32 TimeSec);
        void SetStarRotation(const Vec3& PoleAxis, f32 AngleRad);
        // Lua como 2a luz atmosferica: SkyIllumScale = fracao da iluminancia do SOL usada no
        // scattering do luar (fase/intensidade ja fatoradas pelo chamador); Corona = halo 0..1.
        void SetMoonSkyLight(f32 SkyIllumScale, f32 CoronaIntensity);

        void SetSunDiskHalfAngle(f32 DegHalfAngle);
        void SetSunGlare(f32 Intensity);
        f32  GetSunDiskHalfAngle() const;
        f32  GetSunGlare() const;

        void RecordSkyViewBake(ID3D12GraphicsCommandList* CommandList);
        void RecordAerialPerspectiveBake(ID3D12GraphicsCommandList* CommandList);

        // Cube de reflexo da atmosfera: baka o SkyView LUT num cubemap 64² + prefiltra GGX
        // (MipGen+SpecularPrefilter do IBL) — a água amostra com roughness→mip igual ao HDRI.
        void RecordSkyReflectionBake(ID3D12GraphicsCommandList* CommandList);
        u32  SkyReflectionSRV() const { return SkyReflSpec.SRVSlot(); }

        // Ambient fisico: CS integra o SkyView LUT cos-weighted (ceu + chao virtual) num buffer
        // 2x float4 copiado p/ readback — a CPU le com kFramesInFlight de latencia e escreve nos
        // slots SkyAmbientColor/GroundAmbientColor do FrameConstants (consumidores intactos).
        void RecordSkyAmbientIntegration(ID3D12GraphicsCommandList* CommandList);
        // [0] ceu, [1] chao (modelo de 2 cores), [2..4] SH-L1 por canal (R,G,B).
        static constexpr u32 kAmbientVec4s = 5;
        bool GetSkyAmbient(u32 FrameSlot, Vec3& OutSky, Vec3& OutGround) const;
        // SH-L1 do mesmo integral: OutSH[0]=R, [1]=G, [2]=B, cada um (c0,c1,c2,c3).
        bool GetSkyAmbientSH(u32 FrameSlot, Vec4 OutSH[3]) const;

        void RenderSky(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        void BakeIfDirty(ID3D12Device* Device, FCommandQueue& CmdQueue);
        // Versao que grava na command list DO FRAME (o BakeIfDirty acima faz flush de GPU e so
        // serve fora do frame, na inicializacao). Sem ela o MarkDirty era inerte: nao existia
        // caminho que consumisse o flag depois do Initialize.
        void RecordBakeIfDirty(ID3D12GraphicsCommandList* CommandList);
        void MarkDirty() { Dirty = true; }

        u32 TransmittanceSRV() const { return Transmittance.SRVSlot; }
        u32 MultiScatterSRV()  const { return MultiScatter.SRVSlot; }
        u32 SkyViewSRV()       const { return SkyView.SRVSlot; }
        u32 AerialVolumeSRV()  const { return AerialPerspectiveVolume.SRVSlot(); }
        f32 AerialDepthKm()    const { return CPUConstants.AerialParams.X; }
        f32 AerialSliceCount() const { return CPUConstants.AerialParams.Y; }

        // Fonte UNICA do "raio do observador" e do raio do planeta, em km. Todo consumidor do
        // sky-view LUT fora do AtmosphereCB (hoje: DDGI, ReSTIR GI e reflexoes, via SkyParams no
        // CB de cada um) tem que ler daqui — a parameterizacao do LUT e ancorada na camera por
        // construcao (Hillaire e UE idem), entao raio de GI NAO usa a altitude da propria
        // origem: coerencia com o bake e o ponto todo.
        f32 ViewHeightKm()     const { return CPUConstants.SkyViewSize.Z; }
        f32 BottomRadiusKm()   const { return CPUConstants.PlanetRadii.X; }
        f32 TopRadiusKm()      const { return CPUConstants.PlanetRadii.Y; }
        D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress() const { return CBAddr(); }
        bool IsInitialized() const { return Initialized; }

        void LoadMoonTexture(ID3D12Device* Device, FUploadQueue& UploadQueue,
                             FTextureSRVHeap& SRVHeap, const std::wstring& Path);
        bool HasMoonTexture() const { return MoonTexLoaded; }

        // Catalogo de estrelas real (Assets/Sky/stars.sstars, Yale BSC via HYG). Com ele ativo o
        // hash procedural desliga e RenderStars desenha quads medidos em pixels da saida.
        void LoadStarCatalog(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                             const std::wstring& Path);
        bool HasStarCatalog() const { return StarCount > 0; }
        void RenderStars(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        Vec3 SunTransmittance(const Vec3& DirToSun) const;

    private:
        static constexpr D3D12_RESOURCE_STATES kReadState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        void CreateConstantBuffer(ID3D12Device* Device);
        void BuildInputTables(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void BuildSkyRootSignature(ID3D12Device* Device);
        void BuildStarPipeline(ID3D12Device* Device, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
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

        static constexpr u32 kSkyReflSize    = 64;
        static constexpr u32 kSkyReflMips    = 7;  // 64..1 — casa com kSpecularMaxMip=6 da água
        static constexpr u32 kSkyReflSamples = 64; // céu é liso; 64 taps GGX bastam por frame
        FCubeTexture        SkyReflRaw;   // fonte (SkyView→cube + mip chain)
        FCubeTexture        SkyReflSpec;  // prefiltrado GGX (consumido pela água)
        FVolumetricPipeline SkyReflBakePSO;
        FComputePipeline    SkyReflMipGenPSO;
        FComputePipeline    SkyReflPrefilterPSO;

        FVolumetricPipeline IntegrateAmbientPSO;
        Microsoft::WRL::ComPtr<ID3D12Resource> AmbientBuffer;   // DEFAULT, 2x float4, UAV
        Microsoft::WRL::ComPtr<ID3D12Resource> AmbientReadback; // READBACK ring (kFramesInFlight)
        u8* AmbientMapped   = nullptr;
        u32 AmbientUAVSlot  = 0;
        u32 AmbientRecorded = 0; // integracoes gravadas; valido apos >= kFramesInFlight

        u32 SkyViewBakeTableStart = 0;
        u32 SkyRenderTableStart   = 0;

        DXGI_FORMAT SkyRTFormat = DXGI_FORMAT_UNKNOWN; // formatos do sky pass (p/ PSOs tardios)
        DXGI_FORMAT SkyDSFormat = DXGI_FORMAT_UNKNOWN;

        Microsoft::WRL::ComPtr<ID3D12Resource>      StarBuffer; // upload heap, N x FStar (20B)
        u32                                         StarCount = 0;
        u32                                         StarTableStart = 0; // [stars SRV, transmittance SRV]
        Microsoft::WRL::ComPtr<ID3D12RootSignature> StarRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> StarPSO;

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
