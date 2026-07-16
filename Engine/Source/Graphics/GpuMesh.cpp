#include "Smile/Graphics/GpuMesh.h"
#include "Smile/Graphics/VramTracker.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>

namespace Smile {
    namespace {
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(
            ID3D12Device* _Device, const void* _Src, UINT _Size) {
            D3D12_HEAP_PROPERTIES HeapProps{};
            HeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC ResourceDesc{};
            ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            ResourceDesc.Width            = _Size;
            ResourceDesc.Height           = 1;
            ResourceDesc.DepthOrArraySize = 1;
            ResourceDesc.MipLevels        = 1;
            ResourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
            ResourceDesc.SampleDesc       = { 1, 0 };
            ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

            Microsoft::WRL::ComPtr<ID3D12Resource> Buffer;
            SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                     &ResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                     IID_PPV_ARGS(&Buffer)));

            D3D12_RANGE NoReadRange{ 0, 0 };
            void* Mapped = nullptr;
            SMILE_HR(Buffer->Map(0, &NoReadRange, &Mapped));
            std::memcpy(Mapped, _Src, _Size);
            Buffer->Unmap(0, nullptr);
            return Buffer;
        }
    }

    void FGpuMesh::Upload(ID3D12Device* _Device, const FMesh& _Mesh) {
        const UINT VertexBufferSize = static_cast<UINT>(_Mesh.Vertices.size() * sizeof(Vertex));
        const UINT IndexBufferSize  = static_cast<UINT>(_Mesh.Indices.size()  * sizeof(u32));
        IndexCount = static_cast<u32>(_Mesh.Indices.size());

        VertexBuffer = CreateUploadBuffer(_Device, _Mesh.Vertices.data(), VertexBufferSize);
        VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VertexBufferView.StrideInBytes  = sizeof(Vertex);
        VertexBufferView.SizeInBytes    = VertexBufferSize;

        IndexBuffer = CreateUploadBuffer(_Device, _Mesh.Indices.data(), IndexBufferSize);
        IndexBufferView.BufferLocation = IndexBuffer->GetGPUVirtualAddress();
        IndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
        IndexBufferView.SizeInBytes    = IndexBufferSize;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> FGpuMesh::CreatePoolBuffer(ID3D12Device* _Device,
                                                                      u64 _Size) {
        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC ResourceDesc{};
        ResourceDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        ResourceDesc.Width            = _Size;
        ResourceDesc.Height           = 1;
        ResourceDesc.DepthOrArraySize = 1;
        ResourceDesc.MipLevels        = 1;
        ResourceDesc.Format           = DXGI_FORMAT_UNKNOWN;
        ResourceDesc.SampleDesc       = { 1, 0 };
        ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ResourceDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3D12Resource> Buffer;
        SMILE_HR(_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE,
                 &ResourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                 IID_PPV_ARGS(&Buffer)));
        VramTracker::Register(Buffer.Get(), EVramCategory::Geometry);
        return Buffer;
    }

    void FGpuMesh::InitFromPool(const Microsoft::WRL::ComPtr<ID3D12Resource>& _Pool,
                                u64 _VbOffset, u64 _IbOffset,
                                u32 _VertexCount, u32 _IndexCount) {
        IndexCount = _IndexCount;
        if (_VertexCount == 0 || _IndexCount == 0) { IndexCount = 0; return; }

        VertexBuffer = _Pool;
        IndexBuffer  = _Pool;
        const D3D12_GPU_VIRTUAL_ADDRESS Base = _Pool->GetGPUVirtualAddress();
        VertexBufferView.BufferLocation = Base + _VbOffset;
        VertexBufferView.StrideInBytes  = sizeof(Vertex);
        VertexBufferView.SizeInBytes    = _VertexCount * static_cast<u32>(sizeof(Vertex));
        IndexBufferView.BufferLocation  = Base + _IbOffset;
        IndexBufferView.Format          = DXGI_FORMAT_R32_UINT;
        IndexBufferView.SizeInBytes     = _IndexCount * static_cast<u32>(sizeof(u32));
        VbFirstElement = static_cast<u32>(_VbOffset / sizeof(Vertex));
        IbFirstElement = static_cast<u32>(_IbOffset / sizeof(u32));
    }

    void FGpuMesh::Draw(ID3D12GraphicsCommandList* _CommandList) const {
        if (IndexCount == 0) return;
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
        _CommandList->IASetIndexBuffer(&IndexBufferView);
        _CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);
    }
}
