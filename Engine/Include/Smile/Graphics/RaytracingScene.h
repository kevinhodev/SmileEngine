#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <unordered_map>

namespace Smile {
    class FD3D12Device;
    class FCommandQueue;
    class FTextureSRVHeap;
    class FScene;
    class FGpuMesh;

    class FRaytracingScene {
    public:
        void Build(FD3D12Device& Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap,
                   const FScene& Scene);

        // Rebuild SO da TLAS (BLAS/pool intactos) na command list do FRAME — p/ transform de
        // renderable mudado no editor (gizmo). Rebuild em vez de update (recomendacao
        // NVIDIA/UE p/ TLAS); in-place: mesmo buffer/VA, o SRV segue valido. Retorna false
        // se nao ha TLAS ou se ha mais instancias que a capacidade do load (ai so o Build
        // completo resolve — NAO pode ser chamado mid-frame).
        bool RecordTlasRebuild(ID3D12GraphicsCommandList4* CL, const FScene& Scene,
                               u32 FrameSlot);

        void Release(FTextureSRVHeap& SRVHeap);

        // Upload de instancias versionado por frame em voo (CPU escreve enquanto a GPU
        // ainda le o slice do frame anterior).
        static constexpr u32 kInstanceSlots = 2; // == FCommandQueue::kFramesInFlight (assert no .cpp)

        bool IsBuilt()        const { return Built; }
        u32  TlasSRVSlot()    const { return TlasSRVSlot_; }
        u32  InstanceCount()  const { return InstanceCount_; }
        u32  BlasCount()      const { return BlasCount_; }

        // === Snapshot InstanceGeo =========================================================
        // Material + geometria por instancia, indexado pelo InstanceID da TLAS: o que TODO hit
        // de RT le para saber em que superficie bateu (VB/IB bindless, albedo, MR, emissivo,
        // alpha cutoff, two-sided). Consumido por DDGI, ReSTIR GI/DI, reflexoes, mesh lights,
        // temporal motion, BvhDebug e — a partir da serie SHaRC — pelo updater do radiance cache.
        //
        // Morava no FDDGI, e isso obrigava sete consumidores que nao tocam em sonda nenhuma a
        // esperar o VOLUME existir para receber um descritor valido. Aqui ele fica ao lado do que
        // ja descreve a mesma cena de RT: a TLAS enderecada pelo mesmo InstanceID e o BlasByMesh,
        // que percorre exatamente a mesma lista de malhas unicas deste snapshot.
        u32  InstanceGeoSRV()      const { return InstanceGeoSRVSlot_; }
        // Quantas instancias o snapshot consegue descrever. Alocado no Build pelo tamanho da cena
        // de entao, COM folga (SceneCapacityFor); o RefreshInstanceGeo trunca nele. Quem cria
        // objeto com a cena ja carregada precisa consultar antes (Renderer::OnSceneStructureChanged).
        u32  InstanceGeoCapacity() const { return InstanceGeoCount; }
        // Re-upload do snapshot sem refazer o Build. Chamar quando uma propriedade de material que
        // o RT enxerga muda em runtime (AlphaTest, TwoSided, emissivo...) ou quando a lista de
        // renderaveis muda e ainda cabe na folga: o snapshot e criado uma vez e nao acompanha a
        // edicao sozinho.
        //
        // ⚠️ CONTRATO: o chamador drena as DUAS filas antes — Flush na direta e WaitIdle na
        // compute, nesta ordem. E upload heap sem versao por frame em voo, entao escrever com
        // trabalho em voo corrompe o que ele le; e o consumidor nao esta so na direta, porque o
        // update do DDGI roda na COMPUTE e chega ao snapshot pela tabela de trace dele. Drenar so
        // a direta deixa passar exatamente esse leitor. Nao da para drenar aqui dentro: as filas
        // sao do Renderer.
        void RefreshInstanceGeo(const FScene& Scene);
        // Teto de instancias que os rebuilds por frame conseguem endereçar: TLAS, scratch e
        // uploads foram dimensionados para ele no Build. Passar disso exige Build de novo —
        // quem cria objeto com a cena carregada precisa consultar (Renderer::OnSceneStructureChanged).
        u32  InstanceCapacity() const { return InstanceCapacity_; }

        // A TLAS descreve a GEOMETRIA: TRIANGLE_CULL_DISABLE sai apenas nas instancias que sao
        // mesmo two-sided (FMaterial::IsTwoSidedForRT). Nao ha mais chave global — quem decide se
        // culla e cada PASSE, pela ray flag, como no Lumen. Antes isto era um toggle, e ligar ou
        // desligar mexia em TODOS os passes de uma vez: eixo errado.

    private:
        void CollectInstances(const FScene& Scene,
                              std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& Out) const;
        // Definido no .cpp (FRTInstanceGeo e local daquele arquivo); _Mapped tem _Count entradas.
        void FillInstanceGeo(const FScene& Scene, u8* Mapped, u32 Count) const;

        bool   Built          = false;
        u32    TlasSRVSlot_    = 0xFFFFFFFFu;
        u32    InstanceCount_  = 0;
        u32    BlasCount_      = 0;

        // Todos os BLAS vivem suballocados (offsets 256B) num buffer unico ja compactado;
        // o map guarda o VA de cada BLAS dentro do pool.
        Microsoft::WRL::ComPtr<ID3D12Resource>                         BlasPool;
        std::unordered_map<const FGpuMesh*, D3D12_GPU_VIRTUAL_ADDRESS> BlasByMesh;
        Microsoft::WRL::ComPtr<ID3D12Resource>                         Tlas;

        // Infra persistente do rebuild de TLAS por frame: uploads de instancias + scratch
        // dimensionado no load p/ a capacidade maxima (todos os renderables).
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceUpload[kInstanceSlots];
        u8*                                    InstanceMapped[kInstanceSlots] = {};
        Microsoft::WRL::ComPtr<ID3D12Resource> TlasScratch;
        u32                                    InstanceCapacity_ = 0;

        // Snapshot InstanceGeo (ver os acessores acima). Upload heap sem versao por frame: o
        // conteudo muda raramente e sempre com a GPU drenada pelo chamador.
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceGeoBuf;
        u32                                    InstanceGeoCount    = 0; // capacidade, nao a cena
        u32                                    InstanceGeoSRVSlot_ = 0xFFFFFFFFu;
        // SRVs bindless de VB/IB por malha unica (2 slots contiguos por malha: base+2i = VB,
        // base+2i+1 = IB) — o InstanceGeo carrega os indices. Mesma lista de malhas unicas do
        // BlasByMesh, agora percorrida UMA vez: o DDGI refazia a propria deduplicacao com o
        // mesmo criterio, e duas listas com a mesma regra sao uma que fica para tras.
        u32                                    MeshGeoSlotBase  = 0xFFFFFFFFu;
        u32                                    MeshGeoSlotCount = 0;
        // Malha -> slot bindless do VB (IB = +1). Membro, e nao local do Build, porque o
        // RefreshInstanceGeo remonta o snapshot sem refazer a alocacao de descritores.
        std::unordered_map<const FGpuMesh*, u32> MeshGeoSlot;
    };
}
