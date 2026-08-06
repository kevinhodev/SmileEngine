#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/CommandQueue.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FScene;
    class FGpuProfiler;
    class FTextureSRVHeap;

    // Dados compartilhados pelos vetores temporais especificos do cap. 25 de Ray Tracing Gems II.
    // O buffer e indexado pelo InstanceID da TLAS (que, na Smile, e o indice do FRenderable).
    struct FTemporalInstanceTransformGPU {
        Mat44 CurrentToPrevious;
        Mat44 PreviousToCurrent;
    };
    static_assert(sizeof(FTemporalInstanceTransformGPU) == 128);

    struct alignas(256) FTemporalMotionConstants {
        Mat44 InvViewProj;
        Mat44 ViewProj;
        Mat44 PrevViewProj;
        Vec4  CameraPos;    // xyz = camera atual
        Vec4  ScreenParams; // width, height, 1/width, 1/height
        Vec4  Params;       // x=frame slot, y=history valid, z=position reject scale, w=instance count
    };
    static_assert(sizeof(FTemporalMotionConstants) == 256);

    // Constrói duas peças de infraestrutura usadas pelos tres casos do capitulo:
    //  1. um historico de superficie (world position + InstanceID) por pixel;
    //  2. um motion vector dual, usado apenas pelos acumuladores de iluminacao.
    //
    // O velocity raster continua sendo a fonte do TAA/DLSS. Reutilizar cor com o vetor dual gera
    // o padrao repetitivo discutido na Fig. 25-5b; ReSTIR/NRD, por outro lado, reutilizam amostras
    // ou radiancia e sao os consumidores corretos.
    class FTemporalMotionVectors {
    public:
        void Initialize(ID3D12Device* Device);

        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                           const FScene& Scene, u32 TlasSlot, u32 InstanceGeoSlot);
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                            u32 Width, u32 Height, u32 DepthSlot, u32 VelocitySlot);

        void UpdatePerFrame(u32 FrameSlot, const FScene& Scene,
                            const Mat44& InvViewProj, const Mat44& ViewProj,
                            const Mat44& PrevViewProj, const Vec3& CameraPos);
        void Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                    FGpuProfiler* Profiler = nullptr);

        void InvalidateHistory() { HistoryValid = false; }
        bool HasHistory() const  { return HistoryValid; }
        bool IsReady() const     { return Ready; }

        u32 MotionSRV() const { return ReliableMotionSRV; }
        u32 TransformSRV(u32 Slot) const {
            return TransformSRVSlot[Slot % FCommandQueue::kFramesInFlight];
        }
        u32 SurfaceSRV(u32 Slot) const {
            return SurfaceSRVSlot[Slot % FCommandQueue::kFramesInFlight];
        }
        u32 InstanceCount() const { return InstanceCount_; }

    private:
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void ReleaseScene(FTextureSRVHeap& SRVHeap);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        static constexpr u32 kFrames = FCommandQueue::kFramesInFlight;
        static_assert(kFrames == 2, "surface history assume ping-pong de dois frames");

        FVolumetricPipeline SurfacePSO; // depth + TLAS + InstanceGeo -> surface history
        FVolumetricPipeline DualPSO;    // raster MV + surface histories + transforms -> reliable MV

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;
        FTemporalMotionConstants CPU{};
        u32 FrameSlot = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> TransformBuffer;
        u8* MappedTransforms = nullptr;
        u32 TransformSRVSlot[kFrames] = { kInvalidSlot, kInvalidSlot };
        std::vector<Mat44> PreviousModels;
        u32 InstanceCount_ = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> Surface[kFrames];
        D3D12_RESOURCE_STATES SurfaceState[kFrames] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        u32 SurfaceSRVSlot[kFrames] = { kInvalidSlot, kInvalidSlot };
        u32 SurfaceUAVSlot[kFrames] = { kInvalidSlot, kInvalidSlot };
        u32 SurfaceTable[kFrames]   = { kInvalidSlot, kInvalidSlot };

        Microsoft::WRL::ComPtr<ID3D12Resource> ReliableMotion;
        D3D12_RESOURCE_STATES ReliableMotionState = D3D12_RESOURCE_STATE_COMMON;
        u32 ReliableMotionSRV = kInvalidSlot;
        u32 ReliableMotionUAV = kInvalidSlot;
        u32 DualTable[kFrames] = { kInvalidSlot, kInvalidSlot };

        u32 TlasSRVSlot = kInvalidSlot;
        u32 InstanceGeoSRVSlot = kInvalidSlot;
        u32 Width = 0, Height = 0;
        bool Initialized = false;
        bool SceneReady = false;
        bool Ready = false;
        bool HistoryValid = false;
        f32 PositionRejectScale = 0.02f;
    };
}
