#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
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
    class FGpuMaterialTable;
    class FGpuMeshTable;
    struct FOpacityMicromapData;

    class FRaytracingScene {
    public:
        // A tabela de materiais entra como PARAMETRO, e nao como membro: o RT e consumidor dela,
        // nao dono (decisao D12 do GPU-DRIVEN-PLAN). O que ele guarda do material passou a ser
        // um par {descritor da tabela, linha} por instancia.
        void Build(FD3D12Device& Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap,
                   const FScene& Scene, const FGpuMaterialTable& Materials,
                   const FGpuMeshTable& Meshes, const Vec3& TlasOrigin);

        // Rebuild SO da TLAS (BLAS/pool intactos) na command list do FRAME — p/ transform de
        // renderable mudado no editor (gizmo). Rebuild em vez de update (recomendacao
        // NVIDIA/UE p/ TLAS); in-place: mesmo buffer/VA, o SRV segue valido. Retorna false
        // se nao ha TLAS ou se ha mais instancias que a capacidade do load (ai so o Build
        // completo resolve — NAO pode ser chamado mid-frame).
        bool RecordTlasRebuild(ID3D12GraphicsCommandList4* CL, const FScene& Scene,
                               const Vec3& TlasOrigin, u32 FrameSlot);

        // Garante que exista BLAS para cada par {malha, micro-map} EXIGIDO agora, construindo o
        // que faltar. Micro-map nulo pede a variante opaca.
        //
        // Uma chamada so cobre as tres transicoes que uma edicao de material produz: micro-map
        // rebakeado (par novo), alpha test desligado (passa a exigir o BLAS opaco, que pode nunca
        // ter sido construido para aquela malha) e alpha test ligado (o inverso).
        //
        // O BLAS novo vai para um recurso PROPRIO, e nao para o BlasPool: o tamanho depende do
        // micro-map, entao ele nao cabe no offset do antigo. O espaco antigo fica orfao no pool
        // ate a proxima carga — memoria parada, medida em OrphanedBlasBytes(). Compactar o pool
        // moveria TODOS os BLAS de endereco e obrigaria a refazer a TLAS inteira, que e mais risco
        // do que o desperdicio justifica.
        //
        // ⚠️ CONTRATO: grava e submete numa command list propria, e espera a fila. O chamador ja
        // drenou (e edicao manual de material). Devolve quantos BLAS foram construidos.
        u32 EnsureBlasFor(FD3D12Device& Device, FCommandQueue& Queue,
                          const std::vector<std::pair<const FGpuMesh*,
                                                      const FOpacityMicromapData*>>& Required);

        // Um par {malha, micro-map} que ninguem mais usa. Some do mapa para o ponteiro do
        // micro-map morto nao virar chave pendurada; o BLAS em si continua vivo no pool ou no
        // recurso proprio, porque frames em voo ainda podem referencia-lo.
        void ForgetBlas(const FGpuMesh* Mesh, const FOpacityMicromapData* Omm);

        void Release(FTextureSRVHeap& SRVHeap);

        // Upload de instancias versionado por frame em voo (CPU escreve enquanto a GPU
        // ainda le o slice do frame anterior).
        static constexpr u32 kInstanceSlots = 2; // == FCommandQueue::kFramesInFlight (assert no .cpp)

        bool IsBuilt()        const { return Built; }
        u32  TlasSRVSlot()    const { return TlasSRVSlot_; }
        u32  InstanceCount()  const { return InstanceCount_; }
        u32  BlasCount()      const { return BlasCount_; }
        bool IsTlasOriginCurrent(const Vec3& Origin) const {
            return TlasOrigin_.X == Origin.X && TlasOrigin_.Y == Origin.Y &&
                   TlasOrigin_.Z == Origin.Z;
        }

        // === Snapshot InstanceGeo =========================================================
        // Material + geometria por instancia, indexado pelo InstanceID da TLAS: o que TODO hit
        // de RT le para saber em que superficie bateu (VB/IB bindless, albedo, MR, emissivo,
        // alpha cutoff, two-sided). Consumido por DDGI, ReSTIR GI/DI, reflexoes, mesh lights,
        // temporal motion, BvhDebug e — a partir da serie SHaRC — pelo updater do radiance cache.
        //
        // Morava no FDDGI, e isso obrigava sete consumidores que nao tocam em sonda nenhuma a
        // esperar o VOLUME existir para receber um descritor valido. Aqui ele fica ao lado do que
        // ja descreve a mesma cena de RT: a TLAS enderecada pelo mesmo InstanceID e o conjunto
        // de malhas geometricas. Uma malha pode ter varios BLAS por causa dos OMM por material.
        u32  InstanceGeoSRV()      const { return InstanceGeoSRVSlot_; }
        // Quantas instancias o snapshot consegue descrever. Alocado no Build pelo tamanho da cena
        // de entao, COM folga (SceneCapacityFor); o RefreshInstanceGeo trunca nele. Quem cria
        // objeto com a cena ja carregada precisa consultar antes (Renderer::OnSceneStructureChanged).
        u32  InstanceGeoCapacity() const { return InstanceGeoCount; }
        // Re-upload do snapshot sem refazer o Build. Depois da Fase 2 isto e necessario apenas
        // quando a ESTRUTURA muda — renderavel criado/removido, malha ou material trocado num
        // renderavel —, e nao mais quando uma PROPRIEDADE de material muda: aquelas vivem na
        // FGpuMaterialTable e sobem por linha suja, sem tocar aqui.
        //
        // ⚠️ CONTRATO: o chamador drena as DUAS filas antes — Flush na direta e WaitIdle na
        // compute, nesta ordem. E upload heap sem versao por frame em voo, entao escrever com
        // trabalho em voo corrompe o que ele le; e o consumidor nao esta so na direta, porque o
        // update do DDGI roda na COMPUTE e chega ao snapshot pela tabela de trace dele. Drenar so
        // a direta deixa passar exatamente esse leitor. Nao da para drenar aqui dentro: as filas
        // sao do Renderer.
        void RefreshInstanceGeo(const FScene& Scene, const FGpuMaterialTable& Materials,
                                const FGpuMeshTable& Meshes);
        // Teto de instancias que os rebuilds por frame conseguem endereçar: TLAS, scratch e
        // uploads foram dimensionados para ele no Build. Passar disso exige Build de novo —
        // quem cria objeto com a cena carregada precisa consultar (Renderer::OnSceneStructureChanged).
        u32  InstanceCapacity() const { return InstanceCapacity_; }

        // A TLAS descreve a GEOMETRIA: TRIANGLE_CULL_DISABLE sai apenas nas instancias que sao
        // mesmo two-sided (FMaterial::IsTwoSidedForRT). Nao ha mais chave global — quem decide se
        // culla e cada PASSE, pela ray flag, como no Lumen. Antes isto era um toggle, e ligar ou
        // desligar mexia em TODOS os passes de uma vez: eixo errado.

    private:
        void CollectInstances(const FScene& Scene, const Vec3& TlasOrigin,
                              std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& Out) const;
        // Definido no .cpp (FRTInstanceGeo e local daquele arquivo); _Mapped tem _Count entradas.
        void FillInstanceGeo(const FScene& Scene, const FGpuMaterialTable& Materials,
                             const FGpuMeshTable& Meshes, u8* Mapped, u32 Count) const;

        bool   Built          = false;
        u32    TlasSRVSlot_    = 0xFFFFFFFFu;
        u32    InstanceCount_  = 0;
        u32    BlasCount_      = 0;

        // OMM e propriedade do par mesh/material. A mesma FGpuMesh pode, portanto, possuir um
        // BLAS opaco e outros BLAS ligados a micro-maps diferentes.
        Microsoft::WRL::ComPtr<ID3D12Resource> BlasPool;
        std::unordered_map<const FGpuMesh*,
            std::unordered_map<const FOpacityMicromapData*, D3D12_GPU_VIRTUAL_ADDRESS>> BlasByMesh;
        // O OMM Array e uma acceleration structure referenciada pelo BLAS e precisa viver pelo
        // mesmo tempo. ArrayData/desc/index sao apenas inputs de build e nao ficam persistentes.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> OmmArrays;
        Microsoft::WRL::ComPtr<ID3D12Resource>                         Tlas;

        // Infra persistente do rebuild de TLAS por frame: uploads de instancias + scratch
        // dimensionado no load p/ a capacidade maxima (todos os renderables).
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceUpload[kInstanceSlots];
        u8*                                    InstanceMapped[kInstanceSlots] = {};
        Microsoft::WRL::ComPtr<ID3D12Resource> TlasScratch;
        u32                                    InstanceCapacity_ = 0;
        // Origem do espaco descrito pelas matrizes da TLAS. O mundo autoral fica intacto; os
        // shaders convertem RayDesc para este espaco e somam a origem ao reconstruir hits.
        Vec3                                   TlasOrigin_{};

        // Snapshot InstanceGeo (ver os acessores acima). Upload heap sem versao por frame: o
        // conteudo muda raramente e sempre com a GPU drenada pelo chamador.
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceGeoBuf;
        u32                                    InstanceGeoCount    = 0; // capacidade, nao a cena
        u32                                    InstanceGeoSRVSlot_ = 0xFFFFFFFFu;
        // BLAS reconstruidos fora do pool pelo EnsureBlasFor. Cada um e um recurso proprio, e o
        // vetor existe para mante-los vivos: o BlasByMesh guarda GPUVA, que nao segura nada.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> RebuiltBlas;
        u64 RebuiltBlasBytes_ = 0;

        // Os SRVs bindless de geometria por malha unica ([VB][IB][RTTri]) e a enumeracao das
        // malhas unicas saíram daqui na Fase 2 e vivem na FGpuMeshTable. Ver a nota no Build.
    public:
        // VRAM que os rebakes acrescentaram: os BLAS construidos FORA do pool.
        //
        // E esta a grandeza que cresce, e nao o espaco que sobra orfao dentro do pool — o pool ja
        // estava alocado no tamanho dele, entao tornar um pedaco inalcancavel nao custa um byte a
        // mais. Publicado porque o modo de falha e crescimento silencioso: uma sessao longa de
        // edicao de cutoff acumula um BLAS por rebake, e sem numero isso so apareceria como VRAM
        // a mais muito depois. Zera na proxima carga de cena, que refaz o pool.
        u64 RebuiltBlasBytes() const { return RebuiltBlasBytes_; }
        u32 RebuiltBlasCount() const { return static_cast<u32>(RebuiltBlas.size()); }
    };
}
