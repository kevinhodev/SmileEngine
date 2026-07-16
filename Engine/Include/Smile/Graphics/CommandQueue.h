#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    class FCommandQueue {
    public:
        static constexpr u32 kFramesInFlight = 2;

        void Initialize(ID3D12Device* Device, D3D12_COMMAND_LIST_TYPE Type);

        void ResetForRecording();
        void ExecuteAndSync(ID3D12CommandList* const* CommandLists, UINT Count);
        void Flush();

        void BeginFrame();
        void EndFrame(ID3D12CommandList* const* CommandLists, UINT Count);

        // Async compute: Wait/Signal de fila so operam ENTRE ExecuteCommandLists, entao
        // o frame vira segmentos — fecha e executa o que foi gravado, sinaliza a fence e
        // reabre a MESMA list no allocator do frame. Retorna o valor sinalizado (a fila
        // de compute espera esse valor antes de rodar).
        u64  SubmitSegmentAndContinue();
        // Esta fila espera (GPU-side) uma fence externa — vale pros proximos ECLs.
        void GpuWait(ID3D12Fence* ExternalFence, u64 Value);
        ID3D12Fence* NativeFence() const { return Fence.Get(); }

        u32 FrameIndex() const { return CurrentFrame; }

        ID3D12CommandQueue*        Native()    const { return CommandQueue.Get(); }
        ID3D12GraphicsCommandList* List()      const { return CommandList.Get(); }

    private:
        void Signal();                 
        void WaitForValue(UINT64 Value);

        ComPtr<ID3D12CommandQueue>        CommandQueue;
        ComPtr<ID3D12CommandAllocator>    UploadAllocator;
        ComPtr<ID3D12CommandAllocator>    FrameAllocators[kFramesInFlight];
        ComPtr<ID3D12GraphicsCommandList> CommandList;
        ComPtr<ID3D12Fence>               Fence;

        UINT64 FenceValue = 0;
        UINT64 FrameFenceValues[kFramesInFlight] = {};
        u32    CurrentFrame = kFramesInFlight - 1; 
        HANDLE FenceEvent   = nullptr;
    };
}
