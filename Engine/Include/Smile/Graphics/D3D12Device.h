#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include "Smile/Core/Types.h"

namespace Smile {

class D3D12Device {
public:
    void Initialize(bool enableDebugLayer);

    ID3D12Device*   Native() const { return Device.Get(); }
    IDXGIFactory6*  GetFactory() const { return Factory.Get(); }
    IDXGIAdapter1*  GetAdapter() const { return Adapter.Get(); }

    bool TearingSupported() const { return IsTearingSupported; }

    const std::wstring& GetAdapterDescription() const { return AdapterDescription; }
    u64 GetAdapterDedicatedVideoMemory() const { return AdapterDedicatedVideoMemory; }

private:
    ComPtr<ID3D12Device>  Device;
    ComPtr<IDXGIFactory6> Factory;
    ComPtr<IDXGIAdapter1> Adapter;
    std::wstring          AdapterDescription;
    u64                   AdapterDedicatedVideoMemory = 0;
    bool                  IsTearingSupported = false;
};

} // namespace Smile
