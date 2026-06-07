#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"

namespace Smile {
    // Fila de comandos com dois modos de uso, compartilhando 1 command list e 1 fence:
    //
    //  - Sincrono (uploads de init, resize): ResetForRecording -> grava ->
    //    ExecuteAndSync. Usa um allocator dedicado e ESPERA a GPU terminar.
    //
    //  - Per-frame (frames in flight): BeginFrame -> grava -> EndFrame. Usa um ring
    //    de N allocators e NAO espera no Execute; a espera acontece no BeginFrame do
    //    frame que vai reutilizar aquele allocator (N frames atras), permitindo que
    //    CPU e GPU trabalhem em paralelo.
    class FCommandQueue {
    public:
        static constexpr u32 kFramesInFlight = 2;

        void Initialize(ID3D12Device* Device, D3D12_COMMAND_LIST_TYPE Type);

        // --- Caminho sincrono ---
        void ResetForRecording();
        void ExecuteAndSync(ID3D12CommandList* const* CommandLists, UINT Count);
        void Flush();

        // --- Caminho per-frame (frames in flight) ---
        // Avanca o ring, espera o allocator deste slot liberar e reseta list+allocator.
        void BeginFrame();
        // Executa e sinaliza a fence deste frame, SEM esperar.
        void EndFrame(ID3D12CommandList* const* CommandLists, UINT Count);
        // Indice do frame corrente no ring [0, kFramesInFlight). Usado para indexar
        // recursos por-frame (constant buffers double-buffered).
        u32 FrameIndex() const { return CurrentFrame; }

        ID3D12CommandQueue*        Native()    const { return CommandQueue.Get(); }
        ID3D12GraphicsCommandList* List()      const { return CommandList.Get(); }

    private:
        void Signal();                 // ++FenceValue; CommandQueue->Signal(FenceValue)
        void WaitForValue(UINT64 Value);

        ComPtr<ID3D12CommandQueue>        CommandQueue;
        // Allocator do caminho sincrono (seguro reusar pois ExecuteAndSync espera).
        ComPtr<ID3D12CommandAllocator>    UploadAllocator;
        // Ring de allocators do caminho per-frame.
        ComPtr<ID3D12CommandAllocator>    FrameAllocators[kFramesInFlight];
        ComPtr<ID3D12GraphicsCommandList> CommandList;
        ComPtr<ID3D12Fence>               Fence;

        UINT64 FenceValue = 0;
        // Valor de fence sinalizado da ultima vez que cada allocator do ring foi usado.
        UINT64 FrameFenceValues[kFramesInFlight] = {};
        u32    CurrentFrame = kFramesInFlight - 1; // primeiro BeginFrame -> 0
        HANDLE FenceEvent   = nullptr;
    };
}
