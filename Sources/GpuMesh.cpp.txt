#include "Smile/Graphics/GpuMesh.h"
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

    namespace {
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(
            ID3D12Device* _Device, UINT _Size) {
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
            return Buffer;
        }
    }

    void FGpuMesh::RecordUploadDefault(ID3D12Device* _Device,
                                       ID3D12GraphicsCommandList* _CommandList,
                                       const FMesh& _Mesh,
                                       std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& _StagingOut) {
        const UINT VertexBufferSize = static_cast<UINT>(_Mesh.Vertices.size() * sizeof(Vertex));
        const UINT IndexBufferSize  = static_cast<UINT>(_Mesh.Indices.size()  * sizeof(u32));
        IndexCount  = static_cast<u32>(_Mesh.Indices.size());
        DefaultHeap = true; 
        if (VertexBufferSize == 0 || IndexBufferSize == 0) { IndexCount = 0; return; }

        VertexBuffer = CreateDefaultBuffer(_Device, VertexBufferSize);
        IndexBuffer  = CreateDefaultBuffer(_Device, IndexBufferSize);

        auto VStage = CreateUploadBuffer(_Device, _Mesh.Vertices.data(), VertexBufferSize);
        auto IStage = CreateUploadBuffer(_Device, _Mesh.Indices.data(),  IndexBufferSize);

        _CommandList->CopyBufferRegion(VertexBuffer.Get(), 0, VStage.Get(), 0, VertexBufferSize);
        _CommandList->CopyBufferRegion(IndexBuffer.Get(),  0, IStage.Get(), 0, IndexBufferSize);

        D3D12_RESOURCE_BARRIER Barriers[2]{};
        for (int i = 0; i < 2; ++i) {
            Barriers[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            Barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        Barriers[0].Transition.pResource  = VertexBuffer.Get();
        Barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        Barriers[1].Transition.pResource  = IndexBuffer.Get();
        Barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        _CommandList->ResourceBarrier(2, Barriers);

        VertexBufferView.BufferLocation = VertexBuffer->GetGPUVirtualAddress();
        VertexBufferView.StrideInBytes  = sizeof(Vertex);
        VertexBufferView.SizeInBytes    = VertexBufferSize;
        IndexBufferView.BufferLocation  = IndexBuffer->GetGPUVirtualAddress();
        IndexBufferView.Format          = DXGI_FORMAT_R32_UINT;
        IndexBufferView.SizeInBytes     = IndexBufferSize;

        _StagingOut.push_back(std::move(VStage));
        _StagingOut.push_back(std::move(IStage));
    }

    void FGpuMesh::Draw(ID3D12GraphicsCommandList* _CommandList) const {
        if (IndexCount == 0) return;
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
        _CommandList->IASetIndexBuffer(&IndexBufferView);
        _CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);
    }
}
