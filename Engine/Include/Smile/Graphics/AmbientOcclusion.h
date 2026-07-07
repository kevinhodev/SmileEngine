#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    struct alignas(256) GTAOConstants {
        Vec4 ProjA;        // x=M00, y=M11, z=M22, w=M32 (termos da projecao LH)
        Vec4 ScreenParams; // x=W, y=H, z=1/W, w=1/H
        Vec4 Params;       // x=radius (mundo), y=intensity, z=power, w=falloffEnd (mundo)
        Vec4 Params2;      // x=numDir, y=numSteps, z=frameIndex, w=blurDir (0=H,1=V)
        Vec4 Params3;      // x=fadeStart (Z de view), y=fadeEnd, zw=reservado
        Mat44 ViewMatrix;  // world->view: transforma a normal do G-buffer p/ view (GTAO main)
    };

    class FAmbientOcclusion {
    public:
        void Initialize(ID3D12Device* Device);

        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                            u32 DepthSRVSlot, u32 NormalSRVSlot, u32 Width, u32 Height);

        void UpdatePerFrame(u32 FrameSlot, f32 M00, f32 M11, f32 M22, f32 M32,
                            const Mat44& View, u32 Width, u32 Height, u32 FrameIndex);

        void Execute(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap,
                     u32 DepthSRVSlot);

        void ClearToWhite(ID3D12GraphicsCommandList* CommandList, FTextureSRVHeap& SRVHeap);

        bool IsReady() const     { return Ready; }
        u32  AOSRVSlot() const   { return AO0SRVSlot; } 

        void SetRadius(f32 V)    { Radius = V; }
        f32  GetRadius() const   { return Radius; }
        void SetIntensity(f32 V) { Intensity = V; }
        f32  GetIntensity() const{ return Intensity; }
        void SetPower(f32 V)     { Power = V; }
        f32  GetPower() const    { return Power; }
        void SetFadeStart(f32 V) { FadeStart = V; }
        f32  GetFadeStart() const{ return FadeStart; }
        void SetFadeEnd(f32 V)   { FadeEnd = V; }
        f32  GetFadeEnd() const  { return FadeEnd; }
        // Meia-res (estilo UE): main+blur em W/2 x H/2 e upsample bilateral p/ o AO0
        // full-res final. As duas cadeias ficam alocadas; o toggle e por frame.
        void SetHalfRes(bool V)  { HalfRes = V; }
        bool GetHalfRes() const  { return HalfRes; }

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseSizedResources(FTextureSRVHeap& SRVHeap);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Res,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);

        D3D12_GPU_VIRTUAL_ADDRESS CBAddr(u32 Variant) const {
            return CB->GetGPUVirtualAddress() +
                   static_cast<UINT64>(FrameSlot * 2 + Variant) * sizeof(GTAOConstants);
        }

        FVolumetricPipeline MainPSO;
        FVolumetricPipeline BlurPSO;
        FVolumetricPipeline UpsamplePSO;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        Microsoft::WRL::ComPtr<ID3D12Resource> AO0; // full-res, SEMPRE o final (t14 do lighting)
        Microsoft::WRL::ComPtr<ID3D12Resource> AO1; // full-res, ping-pong do blur em modo full
        Microsoft::WRL::ComPtr<ID3D12Resource> AOH0; // meia-res, ping-pong do modo half
        Microsoft::WRL::ComPtr<ID3D12Resource> AOH1;
        u32 AO0SRVSlot     = kInvalidSlot;
        u32 AO0UAVSlot     = kInvalidSlot;
        u32 AO1SRVSlot     = kInvalidSlot;
        u32 AO1UAVSlot     = kInvalidSlot;
        u32 AOH0SRVSlot    = kInvalidSlot;
        u32 AOH0UAVSlot    = kInvalidSlot;
        u32 AOH1SRVSlot    = kInvalidSlot;
        u32 AOH1UAVSlot    = kInvalidSlot;
        u32 MainTable      = kInvalidSlot;
        u32 BlurTable0     = kInvalidSlot;
        u32 BlurTable1     = kInvalidSlot;
        u32 BlurTableH0    = kInvalidSlot; // depth + AOH0 (blur H e input do upsample)
        u32 BlurTableH1    = kInvalidSlot; // depth + AOH1 (blur V)

        D3D12_RESOURCE_STATES AO0State  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES AO1State  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES AOH0State = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES AOH1State = D3D12_RESOURCE_STATE_COMMON;
        static constexpr D3D12_RESOURCE_STATES kAORead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D12Resource> CB; 
        u8* MappedCB  = nullptr;
        u32 FrameSlot = 0;
        u32 Width = 0, Height = 0;
        GTAOConstants CPU{};

        f32 Radius    = 0.6f; 
        f32 Intensity = 1.0f;
        f32 Power     = 1.1f;
        int NumDir    = 3;     
        int NumSteps  = 8;

        f32 FadeStart = 50.0f;
        f32 FadeEnd   = 300.0f;

        bool HalfRes = true;

        bool Initialized = false;
        bool Ready       = false; 
    };
}
