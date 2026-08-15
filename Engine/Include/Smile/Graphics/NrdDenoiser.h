#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace nrd { struct Instance; }

namespace Smile {
    class FNrdDenoiser {
    public:
        enum class ESignalProfile : u8 { Indirect = 0, Direct = 1 };

        enum EIo {
            IO_MV = 0, IO_NORMAL_ROUGHNESS, IO_VIEWZ,
            IO_DIFF_RADIANCE_HITDIST, IO_SPEC_RADIANCE_HITDIST,
            IO_OUT_DIFF, IO_OUT_SPEC, IO_COUNT
        };

        void Initialize(ID3D12Device* Device,
                        ESignalProfile Profile = ESignalProfile::Indirect);
        void Shutdown();
        bool IsAvailable() const { return Available; }

        void SetupForResize(ID3D12Device* Device, u32 Width, u32 Height);
        // Devolve pools + IO (o grosso da VRAM) sem derrubar pipelines, root signature nem os
        // descriptor heaps — reallocar depois e so um SetupForResize. Existe para o Renderer
        // poder manter a instancia viva mas SEM memoria enquanto o denoiser nao e o NRD ou o
        // consumidor (ReSTIR GI/DI) esta desligado. Chamar so com a GPU ociosa.
        void ReleaseResources() { ReleaseResize(); }
        bool IsReady() const { return Ready; }

        // Per frame: matrizes NAO jitteradas (column-major p/ o NRD = memcpy direto da row-major da
        // engine, lida transposta) + jitter + frameIndex -> nrd::SetCommonSettings.
        void SetFrame(const Mat44& ViewToClip, const Mat44& ViewToClipPrev,
                      const Mat44& WorldToView, const Mat44& WorldToViewPrev,
                      const Vec2& Jitter, const Vec2& JitterPrev, u32 FrameIndex);

        void Denoise(ID3D12GraphicsCommandList* CommandList);

        void InvalidateHistory() { NeedsClear = true; }

        void TransitionInputsToWrite(ID3D12GraphicsCommandList* CL);
        void TransitionOutputToRead(ID3D12GraphicsCommandList* CL);

        ID3D12Resource* IoResource(EIo Which) const;

        u32 Width() const  { return RtWidth; }
        u32 Height() const { return RtHeight; }

    private:
        struct FNRDTexture {
            Microsoft::WRL::ComPtr<ID3D12Resource> Res;
            D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
            u32 SRVStaging = 0xFFFFFFFFu; // indice na StagingHeap (SRV)
            u32 UAVStaging = 0xFFFFFFFFu; // indice na StagingHeap (UAV)
        };

        void BuildRootSignature(ID3D12Device* Device);
        void BuildPipelines(ID3D12Device* Device);
        void CreateHeapsAndCB(ID3D12Device* Device);
        void ReleaseResize();
        FNRDTexture* MapResource(u32 NRDResourceType, u32 IndexInPool);
        D3D12_CPU_DESCRIPTOR_HANDLE StagingCpu(u32 Index) const;
        void RecordDispatches(ID3D12GraphicsCommandList* CL);

        nrd::Instance* Instance = nullptr;
        ESignalProfile SignalProfile = ESignalProfile::Indirect;

        const char* PoolLabel() const {
            return SignalProfile == ESignalProfile::Direct ? "NRD · Pools (Direta)"
                                                           : "NRD · Pools (Indireta)";
        }

        bool Available = false;
        bool Ready     = false;

        Microsoft::WRL::ComPtr<ID3D12Device>        Device;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> Pipelines;

        std::vector<FNRDTexture> PermanentPool;
        std::vector<FNRDTexture> TransientPool;
        FNRDTexture Io[IO_COUNT];

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> StagingHeap;
        u32 StagingCount = 0;
        u32 HandleSize   = 0;

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> TableHeap;
        u32 TableRingCapacity = 0;
        u32 TableRingOffset   = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBufferRing;
        u8* ConstantBufferRingMapped = nullptr;
        u32 ConstantBufferStride     = 0;
        u32 ConstantBufferRingCount  = 0;
        u32 ConstantBufferRingOffset = 0;

        u32 PerSetTex = 0;
        u32 PerSetUav = 0;
        u32 SetsMax   = 0;
        u32 TableStride = 0;
        u32 ResourcesSpace = 0;
        u32 CbSpace = 1, CbReg = 0, SamplerBaseReg = 0;

        u32 RtWidth = 0, RtHeight = 0;
        bool NeedsClear = false;
    };
}
