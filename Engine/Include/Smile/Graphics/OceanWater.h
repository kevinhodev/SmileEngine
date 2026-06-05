#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    struct FOceanSettings {
        bool Enabled = true;

        f32 WaterLevelY     = 0.0f;
        f32 WindDirectionRad = 0.0f;
        f32 WindSpeed       = 8.0f;
        f32 WaveAmount      = 0.30f;
        f32 WaveSize        = 480.0f;
        f32 Choppiness      = 0.10f;
        f32 SurfaceExtent   = 3000.0f;

        f32 ReflectionScale = 0.45f;
        f32 RefractionScale = 0.35f;
        f32 NormalScale     = 0.75f;
        f32 FoamAmount      = 0.03f;
        f32 FogDensity      = 0.08f;

        Vec3 DeepColor    = { 0.008f, 0.045f, 0.075f };
        Vec3 ShallowColor = { 0.045f, 0.170f, 0.200f };
    };

    class FOceanWater {
    public:
        void Initialize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                        u32 SampleCount, DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Recreate(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void Shutdown();

        void UpdatePerFrame(const Mat44& ViewProj, const Vec3& CameraPos,
                            const Vec3& DirToSun, const Vec3& SunColor,
                            f32 SunIntensity, f32 TimeSeconds,
                            f32 IBLIntensity, f32 IBLRotation,
                            f32 IBLMaxMip, bool IBLEnabled);

        void Render(ID3D12GraphicsCommandList* CommandList,
                    FTextureSRVHeap& SRVHeap,
                    u32 IBLTableStart);

        void SetEnabled(bool Enabled) { Settings.Enabled = Enabled; }
        void SetWaterLevel(f32 LevelY) { Settings.WaterLevelY = LevelY; }
        void SetWind(f32 DirectionRad, f32 Speed);
        void SetWaveParams(f32 WaveAmount, f32 WaveSize, f32 Choppiness);

        const FOceanSettings& GetSettings() const { return Settings; }
        FOceanSettings& GetMutableSettings() { return Settings; }
        bool IsInitialized() const { return Initialized; }

    private:
        static constexpr u32 kDisplacementSize = 64;
        static constexpr u32 kMeshResolution   = 401;
        static constexpr u32 kInvalidSlot      = 0xFFFFFFFFu;

        struct alignas(256) OceanConstants {
            Mat44 ViewProj;
            Vec4  CameraPosition;
            Vec4  SunDirection;
            Vec4  SunColor;
            Vec4  WaterParams0; // x=levelY, y=waveAmount, z=waveSize, w=choppiness
            Vec4  WaterParams1; // x=surfaceExtent, y=normalScale, z=reflScale, w=refrScale
            Vec4  WaterParams2; // x=foamAmount, y=fogDensity, z=time, w=unused
            Vec4  GridOrigin;   // xz center of the camera-snapped grid
            Vec4  DeepColor;
            Vec4  ShallowColor;
            Vec4  IBLParams;    // x=intensity, y=rotation, z=maxMip, w=enabled
            u8    _Pad[256 - 64 - (10 * 16)] = {};
        };
        static_assert(sizeof(OceanConstants) == 256, "OceanConstants must be 256 bytes");

        struct OceanVertex {
            f32 Position[3];
            f32 UV[2];
        };

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device, u32 SampleCount,
                      DXGI_FORMAT RTFormat, DXGI_FORMAT DSFormat);
        void CreateConstantBuffer(ID3D12Device* Device);
        void CreateDisplacementTexture(ID3D12Device* Device, FTextureSRVHeap& SRVHeap);
        void CreateMesh(ID3D12Device* Device);
        void UploadDisplacement(ID3D12GraphicsCommandList* CommandList);

        FOceanSettings Settings;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        OceanConstants* MappedCB = nullptr;

        Microsoft::WRL::ComPtr<ID3D12Resource> DisplacementTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> DisplacementUpload;
        D3D12_RESOURCE_STATES DisplacementState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        u32 DisplacementSRVSlot = kInvalidSlot;
        u32 DisplacementRowPitch = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        u32 IndexCount = 0;

        class FOceanFFT;
        FOceanFFT* FFT = nullptr;

        bool Initialized = false;
    };
}
