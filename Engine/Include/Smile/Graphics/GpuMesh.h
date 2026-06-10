#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Mesh.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    // Buffers GPU de um mesh (vertex + index). Encapsula o que o Renderer fazia
    // inline para a unica esfera; agora reutilizavel por qualquer objeto da cena.
    // Por enquanto vive em upload heap (igual a versao anterior) — mover p/ default
    // heap com copy fica para uma iteracao futura.
    class FGpuMesh {
    public:
        // Sobe os dados CPU de um FMesh para a GPU (upload heap; caminho simples/legado).
        void Upload(ID3D12Device* Device, const FMesh& Mesh);

        // Cria os buffers em DEFAULT heap e GRAVA as copias no command list dado, SEM
        // sincronizar. As staging buffers ficam em StagingOut (manter vivas ate o caller
        // executar+sincronizar o command list). Usado pelo upload em lote da cena (Fase 4):
        // muitos meshes -> 1 sync por chunk, e geometria em default heap (mais rapida na GPU).
        void RecordUploadDefault(ID3D12Device* Device, ID3D12GraphicsCommandList* CommandList,
                                 const FMesh& Mesh,
                                 std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& StagingOut);

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
