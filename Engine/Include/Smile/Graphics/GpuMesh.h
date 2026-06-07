#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Mesh.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    // Buffers GPU de um mesh (vertex + index). Encapsula o que o Renderer fazia
    // inline para a unica esfera; agora reutilizavel por qualquer objeto da cena.
    // Por enquanto vive em upload heap (igual a versao anterior) — mover p/ default
    // heap com copy fica para uma iteracao futura.
    class FGpuMesh {
    public:
        // Sobe os dados CPU de um FMesh para a GPU. Indices em R16_UINT.
        void Upload(ID3D12Device* Device, const FMesh& Mesh);

        // Liga vertex/index buffers e emite o DrawIndexed.
        void Draw(ID3D12GraphicsCommandList* CommandList) const;

        bool IsValid()       const { return IndexCount > 0; }
        u32  GetIndexCount() const { return IndexCount; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_VERTEX_BUFFER_VIEW               VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW                IndexBufferView{};
        u32                                    IndexCount = 0;
    };
}
