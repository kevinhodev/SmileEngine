#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/TextureSRVHeap.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    struct alignas(256) FogConstants {
        Vec4  ExponentialFogParameters;     // x=collapsed density1, y=falloff1, z=maxObserverHeight, w=startDistance
        Vec4  ExponentialFogParameters2;    // x=collapsed density2, y=falloff2, z=density2, w=fogHeight2
        Vec4  ExponentialFogParameters3;    // x=density1, y=fogHeight1, z=unused, w=cutoffDistance
        Vec4  FogInscatteringColor;         // rgb=base color, w=1-maxOpacity
        Vec4  DirectionalInscatteringColor; // rgb=color, w=exponent
        Vec4  InscatteringLightDirection;   // xyz=dir TO sun, w=start distance (>=0 enables)
        Mat44 InvViewProj;                  // FULL inverse view-proj
        Vec4  CameraWorldPos;               // xyz=camera world pos, w=km per world unit
        Vec4  AerialParams;                 // x=AP depth (km), y=useAP, z=useHeightFog, w=shafts volumetricos on
        Vec4  ScreenParams;                 // x=w, y=h, z=1/w, w=1/h
        Vec4  DepthParams;                  // x=near, y=far, z/w unused
    };

    class FFogPass {
    public:
        void Initialize(ID3D12Device* Device, DXGI_FORMAT RTFormat);

        // VolumetricShafts: sun shafts F2 ligados — o shader soma o RT meia-res no
        // inscatter e o termo direcional analitico e desligado (senao dobra a energia).
        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProjFull,
                            const Vec3& CameraWorldPos, f32 KmPerWorldUnit,
                            const Vec3& DirToSun, f32 NearZ, f32 FarZ,
                            u32 Width, u32 Height, bool UseAerial, bool UseHeightFog,
                            f32 AerialDepthKm, bool VolumetricShafts = false);

        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     u32 DepthSRVSlot, u32 AerialVolumeSRVSlot, u32 VolShaftsSRVSlot);

        // Densidades 2-exponenciais colapsadas na altura do observador (mesma conta do
        // UpdatePerFrame) — o raymarch dos sun shafts reconstroi a densidade por altura.
        // Retorna { collapsed1, falloff1, collapsed2, falloff2 }.
        Vec4 CollapsedFogParams(f32 ObserverHeight) const;

        void SetDensity(f32 V)        { Density = V; }
        void SetHeightFalloff(f32 V)  { HeightFalloff = V; }
        void SetFogHeight(f32 V)      { FogHeight = V; }
        void SetFogColor(const Vec3& C){ FogColor = C; }
        void SetMaxOpacity(f32 V)     { MaxOpacity = V; }
        void SetStartDistance(f32 V)  { StartDistance = V; }
        void SetCutoffDistance(f32 V) { CutoffDistance = V; }
        void SetSecondFog(f32 Density2_, f32 Falloff2_, f32 Height2_) {
            Density2 = Density2_; HeightFalloff2 = Falloff2_; FogHeight2 = Height2_;
        }
        void SetDirectionalInscattering(const Vec3& Color, f32 Exponent, f32 StartDist, bool Enabled) {
            DirColor = Color; DirExponent = Exponent; DirStartDistance = StartDist; DirEnabled = Enabled;
        }
        f32  GetDensity() const        { return Density; }
        f32  GetHeightFalloff() const  { return HeightFalloff; }
        f32  GetFogHeight() const      { return FogHeight; }
        Vec3 GetFogColor() const       { return FogColor; }
        f32  GetMaxOpacity() const     { return MaxOpacity; }
        bool IsInitialized() const     { return Initialized; }

    private:
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSOs(ID3D12Device* Device, DXGI_FORMAT RTFormat);
        void CreateConstantBuffer(ID3D12Device* Device);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> PSO;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8*  MappedBase = nullptr;
        u32  FrameSlot  = 0;

        f32  Density        = 0.0002f;
        f32  HeightFalloff  = 0.0005f;
        f32  FogHeight      = 0.0f;
        Vec3 FogColor       = { 0.42f, 0.55f, 0.70f };
        f32  MaxOpacity     = 0.85f;
        f32  StartDistance  = 0.0f;
        f32  CutoffDistance = 0.0f;

        f32  Density2       = 0.0f;
        f32  HeightFalloff2 = 0.0005f;
        f32  FogHeight2     = 0.0f;

        Vec3 DirColor          = { 0.7f, 0.5f, 0.35f };
        f32  DirExponent       = 4.0f;
        f32  DirStartDistance  = 0.0f;
        bool DirEnabled        = true;

        bool Initialized = false;

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const {
            return ConstantBuffer->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot) * sizeof(FogConstants);
        }
        FogConstants* Mapped() const {
            return reinterpret_cast<FogConstants*>(
                MappedBase + static_cast<size_t>(FrameSlot) * sizeof(FogConstants));
        }
    };
}
