#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Graphics/Resources/Mesh.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FGpuMesh {
    public:
        // Sobe VB/IB e o payload de RT por triangulo. Quando o payload nao vem do cozido
        // (primitiva do editor, preview de material) ele e GERADO aqui pelo ResolveRTTriangles —
        // sem isso o caminho procedural ficaria sem payload e os shaders de hit leriam um SRV
        // vazio, o que aparece como normal de face invalida em toda a malha.
        void Upload(ID3D12Device* Device, const FMesh& Mesh);

        // Pool de geometria: varios meshes suballocados num UNICO buffer default-heap
        // ([VB slices desde 0][IB slices]) em vez de 2 committed resources por mesh —
        // committed tem heap de >=64KB, entao mesh pequeno desperdicava VRAM, e criar
        // milhares de recursos dominava o load. Cria o pool (registra no VramTracker);
        // o upload/estado fica com o chamador (FScene::AddMeshesBatch).
        static Microsoft::WRL::ComPtr<ID3D12Resource> CreatePoolBuffer(ID3D12Device* Device,
                                                                       u64 Size);

        // Aponta este mesh pra uma fatia do pool: VBV/IBV/GPUVAs viram base+offset, e
        // VertexBuffer/IndexBuffer seguram o MESMO pool (o refcount do ComPtr mantem o
        // pool vivo enquanto qualquer mesh dele existir). VbOffset PRECISA ser multiplo
        // de sizeof(Vertex), IbOffset de 4 e RtOffset de sizeof(FRTTriangle) (FirstElement
        // dos SRVs bindless e exato). A regiao de RT e a terceira do chunk: [VB][IB][RTTri].
        void InitFromPool(const Microsoft::WRL::ComPtr<ID3D12Resource>& Pool,
                          u64 VbOffset, u64 IbOffset, u64 RtOffset,
                          u32 VertexCount, u32 IndexCount, u32 RtTriangleCount);

        void Draw(ID3D12GraphicsCommandList* CommandList) const;

        // VB/IB sem Draw nem topology. O cache de submissao usa isto para pular
        // IASet* quando o mesh nao mudou; o topology fica a cargo do passe (uma vez).
        void BindIA(ID3D12GraphicsCommandList* CommandList) const;
        void DrawIndexed(ID3D12GraphicsCommandList* CommandList) const;

        bool IsValid()       const { return IndexCount > 0; }
        u32  GetIndexCount() const { return IndexCount; }

        D3D12_GPU_VIRTUAL_ADDRESS VertexBufferGPUVA() const { return VertexBufferView.BufferLocation; }
        D3D12_GPU_VIRTUAL_ADDRESS IndexBufferGPUVA()  const { return IndexBufferView.BufferLocation; }
        u32  VertexCount()  const { return VertexBufferView.StrideInBytes ?
                                           VertexBufferView.SizeInBytes / VertexBufferView.StrideInBytes : 0; }
        u32  VertexStride() const { return VertexBufferView.StrideInBytes; }
        ID3D12Resource* VertexResource() const { return VertexBuffer.Get(); }
        ID3D12Resource* IndexResource()  const { return IndexBuffer.Get(); }

        // Offset da fatia dentro do pool, em ELEMENTOS (Vertex / u32 / FRTTriangle) —
        // FirstElement dos SRVs bindless. 0 quando o mesh tem buffer proprio (Upload).
        u32 VertexFirstElement() const { return VbFirstElement; }
        u32 IndexFirstElement()  const { return IbFirstElement; }

        // Payload de RT por triangulo. O recurso pode ser o MESMO pool do VB/IB (cena cozida) ou
        // um buffer proprio (Upload avulso); quem consome so precisa do par (recurso, elemento).
        // Count 0 = mesh sem payload, e nesse caso nenhum SRV e criado — ver FRaytracingScene.
        ID3D12Resource* RTTriangleResource()     const { return RTTriangleBuffer.Get(); }
        u32             RTTriangleFirstElement() const { return RtFirstElement; }
        u32             RTTriangleCount()        const { return RtTriangleCount_; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        // Terceiro recurso do mesh. Na cena cozida aponta para o mesmo pool que VB/IB (o ComPtr
        // extra so soma refcount); no caminho avulso do Upload e um buffer proprio.
        Microsoft::WRL::ComPtr<ID3D12Resource> RTTriangleBuffer;
        D3D12_VERTEX_BUFFER_VIEW               VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW                IndexBufferView{};
        u32                                    IndexCount     = 0;
        u32                                    VbFirstElement = 0;
        u32                                    IbFirstElement = 0;
        u32                                    RtFirstElement = 0;
        u32                                    RtTriangleCount_ = 0;
    };

    class FMaterial;
    class FTextureSRVHeap;
    // Forward: o cache so guarda um PONTEIRO. Incluir CpuFrameStats.h aqui puxava um header
    // de Renderer para dentro de Resources e, como GpuMesh.h e incluido por quase tudo,
    // espalhava a dependencia pela engine inteira — qualquer mexida na telemetria virava
    // rebuild completo. A definicao entra nos .cpp que tocam nos membros.
    struct FDrawSubmitStats;

    // Cache de submissao por passe: Bind de material e IA de mesh sao idempotentes
    // enquanto o ponteiro nao muda. O CSM ja pulava o Bind; o G-buffer e o prepass
    // reemitiam os dois a cada item, mesmo com a root signature compartilhada.
    // Resetar ao trocar de root signature / DSV / PSO incompativel.
    struct FDrawSubmitCache {
        const FGpuMesh*   LastMesh = nullptr;
        const FMaterial*  LastMat  = nullptr;

        // --- Material BINDLESS (Fase 3) -------------------------------------------------------
        //
        // Ligado, o passe deixa de chamar BindMaterial e passa a chamar BindMaterialRow: o
        // material vira UMA constante de root com o numero da linha, e o pixel shader le a linha
        // canonica pelo heap. O cache continua filtrando repeticao — a lista ja vem agrupada por
        // material, entao a maioria dos itens nao emite nada.
        //
        // Os dois caminhos coexistem no mesmo frame de proposito: passes ainda nao migrados
        // (translucidos, sombras, chuva) continuam classicos, e e a contagem por passe que torna
        // esse fallback visivel em vez de presumido.
        bool Bindless          = false;
        u32  MaterialRootIndex = 0;
        u32  LastRow           = 0xFFFFFFFFu;
        // Material da ULTIMA consulta de linha. `FGpuMaterialTable::RowOf` e um
        // unordered_map, e chama-lo por item poria uma busca por hash no laco de submissao —
        // custo de CPU novo, no caminho que a fase existe para deixar mais barato, e ele
        // apareceria na medicao como se fosse do bindless. As listas ja vem agrupadas por
        // material, entao este ponteiro acerta quase sempre.
        //
        // Separado do LastMat de proposito: aquele descreve o que esta AMARRADO no caminho
        // classico, e os dois caminhos nunca rodam juntos no mesmo passe — reusar o campo
        // faria um significar duas coisas.
        const FMaterial* LastRowMat    = nullptr;
        u32              LastRowLookup = 0;

        // Balde de telemetria do passe (opcional; nullptr = sem instrumentacao). Contar AQUI e
        // deliberado: este e o unico ponto por onde a geometria de cena e submetida, entao o
        // numero nasce do ato de submeter em vez de ser recomposto varrendo listas depois — que
        // e como uma contagem plausivel e errada entraria sem ninguem notar. Ver CpuFrameStats.h.
        FDrawSubmitStats* Stats = nullptr;

        void Reset() {
            LastMesh = nullptr;
            LastMat  = nullptr;
            LastRow  = 0xFFFFFFFFu;
            LastRowMat    = nullptr;
            LastRowLookup = 0;
            // Stats NAO e zerado: o Reset existe para invalidar o cache ao trocar root
            // signature/PSO no MEIO de um passe, e o balde e do passe inteiro.
        }

        void BindMaterial(ID3D12GraphicsCommandList* CommandList,
                          FTextureSRVHeap& SRVHeap, const FMaterial* Mat);

        // Par bindless do BindMaterial: escreve a LINHA do material na constante de root. O
        // descritor da tabela e escrito uma vez por passe e nao passa por aqui.
        //
        // `Mat` entra so para a telemetria poder distinguir "material sem linha" (linha 0 com
        // material nao nulo) de "renderavel sem material" (linha 0 legitima). Os dois produzem o
        // mesmo desenho e so um deles e bug.
        void BindMaterialRow(ID3D12GraphicsCommandList* CommandList, const FMaterial* Mat, u32 Row);
        void DrawMesh(ID3D12GraphicsCommandList* CommandList, const FGpuMesh* Mesh);
    };
}
