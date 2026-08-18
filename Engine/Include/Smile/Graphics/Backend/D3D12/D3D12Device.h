#pragma once

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <chrono>
#include <string>
#include "Smile/Core/Types.h"

namespace Smile {
    // Snapshot de QueryVideoMemoryInfo (IDXGIAdapter3), por processo: LOCAL = VRAM dedicada,
    // NON_LOCAL = RAM compartilhada visivel pela GPU. Budget e o teto dinamico que o OS da ao
    // processo; passar dele (Demoted > 0) significa recursos sendo despejados pra RAM (stutter).
    struct FVideoMemoryInfo {
        u64  LocalUsage     = 0;
        u64  LocalBudget    = 0;
        u64  NonLocalUsage  = 0;
        u64  NonLocalBudget = 0;
        u64  DemotedBytes   = 0;
        bool OverBudget     = false;
        bool Valid          = false;
    };

    class FD3D12Device {
    public:
        ~FD3D12Device() noexcept;

        void Initialize(bool EnableDebugLayer);

        ID3D12Device*   Native() const { return Device.Get(); }
        IDXGIFactory6*  GetFactory() const { return Factory.Get(); }
        IDXGIAdapter1*  GetAdapter() const { return Adapter.Get(); }

        ID3D12Device5*  Device5() const { return DeviceRT.Get(); }

        bool RaytracingSupported() const { return IsRaytracingSupported; }
        bool TearingSupported() const { return IsTearingSupported; }

        // Tier 1 exige heap SEPARADO para buffer, textura comum e textura RT/DS; Tier 2
        // mistura os tres. Sem consumidor hoje (a engine so cria committed, que nao ve heap),
        // mas e o gate de qualquer politica futura de placed resource ou pool — e o valor
        // aparece no log do boot para nao ter de ser adivinhado num relatorio de bug.
        D3D12_RESOURCE_HEAP_TIER HeapTier() const { return ResourceHeapTier; }

        const std::wstring& GetAdapterDescription() const { return AdapterDescription; }
        u64 GetAdapterDedicatedVideoMemory() const { return AdapterDedicatedVideoMemory; }

        // Uso/budget de VRAM do processo. Cacheado por ~500ms (a doc da MS desaconselha query
        // por frame); seguro chamar a cada frame. Retorna o ultimo snapshot valido em falha.
        const FVideoMemoryInfo& QueryVideoMemory() const;

    private:
        ComPtr<ID3D12Device>  Device;
        ComPtr<ID3D12Device5> DeviceRT;
        ComPtr<ID3D12InfoQueue1> DebugInfoQueue;
        ComPtr<IDXGIFactory6> Factory;
        ComPtr<IDXGIAdapter1> Adapter;
        ComPtr<IDXGIAdapter3> Adapter3;

        mutable FVideoMemoryInfo VideoMemoryCache;
        mutable std::chrono::steady_clock::time_point VideoMemoryQueryTime{};
        mutable bool WasOverBudget = false;
        std::wstring          AdapterDescription;
        u64                   AdapterDedicatedVideoMemory = 0;
        bool                  IsTearingSupported    = false;
        bool                  IsRaytracingSupported = false;
        D3D12_RESOURCE_HEAP_TIER ResourceHeapTier   = D3D12_RESOURCE_HEAP_TIER_1;
        DWORD                 DebugCallbackCookie   = 0;
        bool                  DebugCallbackRegistered = false;
    };
} 
