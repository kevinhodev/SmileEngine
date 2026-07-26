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

        // A TLAS descreve a GEOMETRIA: TRIANGLE_CULL_DISABLE sai apenas nas instancias que sao
        // mesmo two-sided (FMaterial::IsTwoSidedForRT). Nao ha mais chave global — quem decide se
        // culla e cada PASSE, pela ray flag, como no Lumen. Antes isto era um toggle, e ligar ou
        // desligar mexia em TODOS os passes de uma vez: eixo errado.

    private:
        void CollectInstances(const FScene& Scene,
                              std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& Out) const;

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
        u32                                    InstanceCapacity = 0;
    };
}
