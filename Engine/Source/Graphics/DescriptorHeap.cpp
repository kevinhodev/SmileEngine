#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Core/HResultCheck.h"

namespace Smile {

void DescriptorHeap::Initialize(ID3D12Device* _Device,
                                D3D12_DESCRIPTOR_HEAP_TYPE _Type,
                                UINT _Capacity,
                                bool _ShaderVisible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type           = _Type;
    desc.NumDescriptors = _Capacity;
    desc.Flags          = _ShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                         : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask       = 0;

    SMILE_HR(_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&DescriptorHeap)));

    HandleSize    = _Device->GetDescriptorHandleIncrementSize(_Type);
    Capacity      = _Capacity;
    ShaderVisible = _ShaderVisible;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CpuHandle(UINT _Index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE Handle = DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    Handle.ptr += static_cast<SIZE_T>(_Index) * HandleSize;
    return Handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GpuHandle(UINT index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE Handle = DescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    Handle.ptr += static_cast<UINT64>(index) * HandleSize;
    return Handle;
}

} // namespace Smile
