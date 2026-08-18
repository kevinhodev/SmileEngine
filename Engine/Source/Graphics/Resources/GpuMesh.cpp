#include "Smile/Graphics/Resources/GpuMesh.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Resources/Material.h"
#include "Smile/Core/HResultCheck.h"
#include <cstring>
#include <vector>

namespace Smile {
    namespace {
        // Caminho avulso do FGpuMesh::Upload (primitivas do editor e do preview de material):
        // o VB/IB fica no proprio upload heap, sem copia para DEFAULT. A cena usa o pool do
        // AddMeshesBatch, nao isto.
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(
            ID3D12Device* _Device, const void* _Src, UINT _Size) {
            const GpuResources::FUploadBuffer Upload =
                GpuResources::CreateUploadBuffer(_Device, _Size, 1, false);
            std::memcpy(Upload.Mapped, _Src, _Size);
            return Upload.Resource;
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

        // Payload de RT tambem por aqui: este caminho serve primitiva do editor e preview de
        // material, que nunca passam pelo .smesh — logo nunca trazem o payload cozido e ele e
        // gerado agora, do mesmo VB/IB que acabou de subir.
        std::vector<FRTTriangle> Scratch;
        const std::vector<FRTTriangle>& Rt = ResolveRTTriangles(_Mesh, Scratch);
        RtTriangleCount_ = static_cast<u32>(Rt.size());
        RtFirstElement   = 0;
        if (RtTriangleCount_ > 0) {
            RTTriangleBuffer = CreateUploadBuffer(
                _Device, Rt.data(), static_cast<UINT>(RtTriangleCount_ * sizeof(FRTTriangle)));
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> FGpuMesh::CreatePoolBuffer(ID3D12Device* _Device,
                                                                      u64 _Size) {
        return GpuResources::CreateBuffer(_Device, _Size, D3D12_RESOURCE_FLAG_NONE,
                                          D3D12_RESOURCE_STATE_COMMON,
                                          EVramCategory::Geometry, "Pool de VB/IB");
    }

    void FGpuMesh::InitFromPool(const Microsoft::WRL::ComPtr<ID3D12Resource>& _Pool,
                                u64 _VbOffset, u64 _IbOffset, u64 _RtOffset,
                                u32 _VertexCount, u32 _IndexCount, u32 _RtTriangleCount) {
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

        // Terceira regiao do MESMO pool: o ComPtr extra so soma refcount, nao aloca nada.
        RtTriangleCount_ = _RtTriangleCount;
        RtFirstElement   = static_cast<u32>(_RtOffset / sizeof(FRTTriangle));
        if (_RtTriangleCount > 0) RTTriangleBuffer = _Pool;
    }

    void FGpuMesh::BindIA(ID3D12GraphicsCommandList* _CommandList) const {
        _CommandList->IASetVertexBuffers(0, 1, &VertexBufferView);
        _CommandList->IASetIndexBuffer(&IndexBufferView);
    }

    void FGpuMesh::DrawIndexed(ID3D12GraphicsCommandList* _CommandList) const {
        _CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);
    }

    void FGpuMesh::Draw(ID3D12GraphicsCommandList* _CommandList) const {
        if (IndexCount == 0) return;
        _CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        BindIA(_CommandList);
        DrawIndexed(_CommandList);
    }

    void FDrawSubmitCache::BindMaterial(ID3D12GraphicsCommandList* _CommandList,
                                        FTextureSRVHeap& _SRVHeap, const FMaterial* _Mat) {
        if (_Mat == LastMat) return;
        LastMat = _Mat;
        if (_Mat) _Mat->Bind(_CommandList, _SRVHeap);
    }

    void FDrawSubmitCache::DrawMesh(ID3D12GraphicsCommandList* _CommandList,
                                    const FGpuMesh* _Mesh) {
        if (!_Mesh || !_Mesh->IsValid()) return;
        if (_Mesh != LastMesh) {
            LastMesh = _Mesh;
            _Mesh->BindIA(_CommandList);
        }
        _Mesh->DrawIndexed(_CommandList);
    }
}
