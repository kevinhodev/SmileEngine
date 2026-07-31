#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include "Smile/Graphics/RayEpsilons.h"
#include "Smile/Graphics/CommandQueue.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;

    // b0 compartilhado pelos passes de Lighting/ReSTIRDI*.cs.hlsl.
    struct alignas(256) ReSTIRDIConstants {
        Mat44 InvViewProj;
        Vec4  CameraPos;
        Vec4  ScreenParams; // W, H, 1/W, 1/H
        Vec4  Params;       // lightCount, frameIndex, shadowMask, rayEndMargin
        Vec4  Sampling;     // initialCandidates, MCap, spatialCount, spatialRadius
        Vec4  Reuse;        // temporal, posRejectScale, normalReject, maxAge
        Vec4  TemporalPolicy; // x = permutation temporal (0/1); yzw reservados
        Vec4  RayEpsA;
        Vec4  RayEpsB;
        Mat44 View;          // world -> view; pack do NRD calcula viewZ linear
    };
    static_assert(sizeof(ReSTIRDIConstants) == 256,
                  "ReSTIRDIConstants deve ocupar uma fatia CBV inteira");

    // ReSTIR DI de superficie primaria para TODAS as luzes locais analiticas. Pass A gera
    // candidatas uniformes e combina o historico; Pass B combina vizinhos e traca no maximo um
    // shadow ray. Materiais emissivos ainda nao sao candidatos: isso exige extrair mesh lights e
    // suas PDFs. O sol fica no caminho dedicado do deferred. O DI-lite permanece separado para A/B.
    class FReSTIRDI {
    public:
        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device);

        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                            u32 Width, u32 Height,
                            u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                            u32 DepthSlot, u32 VelocitySlot, u32 TlasSlot, u32 InstanceSlot,
                            const u32 LightSlots[FCommandQueue::kFramesInFlight]);
        void SetupNrdPack(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                          u32 GBufferASlot, u32 GBufferBSlot, u32 GBufferCSlot,
                          u32 DepthSlot, u32 VelocitySlot,
                          ID3D12Resource* NrdInViewZ, ID3D12Resource* NrdInNormalRough,
                          ID3D12Resource* NrdInMv, ID3D12Resource* NrdInDiffRadHit,
                          ID3D12Resource* NrdInSpecRadHit,
                          ID3D12Resource* NrdOutDiff, ID3D12Resource* NrdOutSpec);

        void UpdatePerFrame(u32 FrameSlot, const Mat44& InvViewProj, const Mat44& View,
                            const Vec3& CameraPos,
                            u32 Width, u32 Height, u32 FrameIndex, u32 LightCount,
                            u32 ShadowRayMask, bool EnableTemporalPermutation);
        void Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);
        void RecordNrdPack(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);
        void RecordNrdComposite(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        void SetRayEpsilons(const FRayEpsilonProfile& P) { RayEps = P; }
        void InvalidateHistory() { NeedsClear = true; }
        void SetLightSetSignature(u64 Signature) {
            if (Signature != LightSetSignature) NeedsClear = true;
            LightSetSignature = Signature;
        }

        bool IsReady() const       { return Ready; }
        bool IsNrdReady() const    { return NrdReady; }
        u32  OutputSRVSlot() const { return OutSRV; }

    private:
        void CreateConstantBuffer(ID3D12Device* Device);
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FVolumetricPipeline InitialTemporalPSO; // 8 SRV, 2 UAV
        FVolumetricPipeline SpatialPSO;         // 9 SRV, 3 UAV, bindless alpha-test
        FVolumetricPipeline NrdPackPSO;          // 7 SRV, 5 UAV
        FVolumetricPipeline NrdCompositePSO;     // 6 SRV, 1 UAV

        Microsoft::WRL::ComPtr<ID3D12Resource> Output;
        Microsoft::WRL::ComPtr<ID3D12Resource> RawDiffuse;  // rgb modulado, a = distancia da luz
        Microsoft::WRL::ComPtr<ID3D12Resource> RawSpecular; // rgb modulado, a = distancia da luz
        Microsoft::WRL::ComPtr<ID3D12Resource> ResA[2]; // RGBA32F: x1.xyz, W
        Microsoft::WRL::ComPtr<ID3D12Resource> ResB[2]; // RGBA16F: light, M+age, n1 oct
        D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES RawDiffuseState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES RawSpecularState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES ResAState[2] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES ResBState[2] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        static constexpr u32 kParityCount = 2;
        u32 OutSRV = kInvalidSlot;
        u32 OutUAV = kInvalidSlot;
        u32 RawDiffuseSRV = kInvalidSlot;
        u32 RawDiffuseUAV = kInvalidSlot;
        u32 RawSpecularSRV = kInvalidSlot;
        u32 RawSpecularUAV = kInvalidSlot;
        u32 ResASRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResBSRV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResAUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 ResBUAV[2] = { kInvalidSlot, kInvalidSlot };
        u32 InitialTable[kParityCount][FCommandQueue::kFramesInFlight] = {
            { kInvalidSlot, kInvalidSlot }, { kInvalidSlot, kInvalidSlot } };
        u32 InitialUAVTable[kParityCount] = { kInvalidSlot, kInvalidSlot };
        u32 SpatialTable[kParityCount][FCommandQueue::kFramesInFlight] = {
            { kInvalidSlot, kInvalidSlot }, { kInvalidSlot, kInvalidSlot } };
        u32 SpatialUAVTable = kInvalidSlot;
        u32 NrdPackSrvTable = kInvalidSlot;
        u32 NrdPackUavTable = kInvalidSlot;
        u32 NrdCompositeSrvTable = kInvalidSlot;
        u32 NrdOutDiffuseSRV = kInvalidSlot;
        u32 NrdOutSpecularSRV = kInvalidSlot;
        static_assert(FCommandQueue::kFramesInFlight == 2,
                      "ajustar inicializadores das tabelas do ReSTIR DI");

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;
        u32 FrameSlot = 0;
        ReSTIRDIConstants CPU{};

        u32 Width = 0, Height = 0;
        u32 FrameParity = 0;
        u64 LightSetSignature = 0;
        bool NeedsClear = true;
        bool Initialized = false;
        bool Ready = false;
        bool NrdReady = false;

        u32 InitialCandidates = 8;
        f32 MCap = 20.0f;
        u32 SpatialCount = 4;
        f32 SpatialRadius = 16.0f;
        f32 PosRejectScale = 0.01f;
        f32 NormalReject = 0.9f;
        f32 MaxAge = 8.0f;
        bool Temporal = true;
        FRayEpsilonProfile RayEps;
    };
}
