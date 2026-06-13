#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Mesh.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FGpuMesh {
    public:
        void Upload(ID3D12Device* Device, const FMesh& Mesh);

        void RecordUploadDefault(ID3D12Device* Device, ID3D12GraphicsCommandList* CommandList,
                                 const FMesh& Mesh,
                                 std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& StagingOut);

        void Draw(ID3D12GraphicsCommandList* CommandList) const;

        bool IsValid()       const { return IndexCount > 0; }
        u32  GetIndexCount() const { return IndexCount; }

        D3D12_GPU_VIRTUAL_ADDRESS VertexBufferGPUVA() const { return VertexBufferView.BufferLocation; }
        D3D12_GPU_VIRTUAL_ADDRESS IndexBufferGPUVA()  const { return IndexBufferView.BufferLocation; }
        u32  VertexCount()  const { return VertexBufferView.StrideInBytes ?
                                           VertexBufferView.SizeInBytes / VertexBufferView.StrideInBytes : 0; }
        u32  VertexStride() const { return VertexBufferView.StrideInBytes; }
        ID3D12Resource* VertexResource() const { return VertexBuffer.Get(); }
        ID3D12Resource* IndexResource()  const { return IndexBuffer.Get(); }

        bool IsDefaultHeap() const { return DefaultHeap; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_VERTEX_BUFFER_VIEW               VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW                IndexBufferView{};
        u32                                    IndexCount  = 0;
        bool                                   DefaultHeap = false; 
    };
}
