#pragma once

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/VolumetricPipeline.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FScene;
    class FTextureSRVHeap;

    // Espelham MeshLightCommon.hlsli. Os static_assert abaixo sao a unica coisa que impede um
    // lado mudar sem o outro e corromper o buffer em silencio.
    struct FMeshLightTaskGPU {
        u32  InstanceIndex;
        u32  TriangleCount;
        u32  LightOffset;
        u32  Pad;
        Vec4 Row0;   // 3 linhas da Mat44 TRANSPOSTA, mesma convencao do TLAS
        Vec4 Row1;
        Vec4 Row2;
    };
    static_assert(sizeof(FMeshLightTaskGPU) == 64);

    struct FTriangleLightGPU {
        Vec3 Base;
        u32  Edges0;
        u32  Edges1;
        u32  Edges2;
        u32  Radiance;
        f32  Flux;
    };
    static_assert(sizeof(FTriangleLightGPU) == 32);

    struct alignas(256) MeshLightConstants {
        u32 NumTasks     = 0;
        u32 NumTriangles = 0;
        u32 Pad0         = 0;
        u32 Pad1         = 0;
    };

    // Levantamento dos triangulos emissivos da cena — primeira fase do projeto de mesh lights.
    //
    // Existe para MEDIR antes de escolher a estrategia de amostragem inicial, porque e a ordem de
    // grandeza da contagem que decide, e nao a intuicao:
    //   centenas          -> proposta uniforme ainda funciona
    //   milhares          -> alias table por potencia vira obrigatoria
    //   dezenas de milhar -> alias table + ReGIR consertado (o pool de 64 slots por celula colapsa
    //                        nessa faixa, ver a auditoria do ReGIR)
    //
    // O que NAO da para levantar aqui: area e fluxo por triangulo. As posicoes dos vertices vivem
    // so na GPU depois do upload (FGpuMesh nao guarda copia na CPU), entao area = |cross(e1,e2)|/2
    // e o fluxo = area*pi*luminancia so saem no passe de extracao. A contagem, que e o numero que
    // destrava a decisao, vem de FGpuMesh::GetIndexCount() e esta disponivel de graca.
    class FMeshLights {
    public:
        struct FStats {
            u32 EmissiveMeshes      = 0; // renderables com material emissivo
            u32 EmissiveTriangles   = 0; // total de triangulos emissivos (o numero que decide)
            u32 MeshesWithMap       = 0; // precisam amostrar textura emissiva na extracao
            u32 TrianglesWithMap    = 0;
            u32 LargestMeshTris     = 0; // maior contribuinte isolado
            u32 TotalRenderables    = 0;
            u32 TotalTriangles      = 0;
        };

        // Percorre a cena e preenche as estatisticas. Barato: so le material e contagem de indices,
        // sem tocar em GPU. Chamado por cena.
        void Survey(const FScene& Scene);

        // Emite o resumo no log, com a leitura do que a contagem implica para a amostragem.
        void LogSummary() const;

        const FStats& Stats() const { return SceneStats; }
        u32  EmissiveTriangleCount() const { return SceneStats.EmissiveTriangles; }
        bool HasEmissiveGeometry()   const { return SceneStats.EmissiveTriangles > 0; }

        void Initialize(ID3D12Device* Device);
        void RecreatePSO(ID3D12Device* Device);

        // Faz o Survey, monta as tasks e cria os buffers. Precisa do SRV do InstanceGeo do DDGI,
        // que e onde moram VB/IB bindless e os dados de material emissivo.
        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap, const FScene& Scene,
                           u32 InstanceSlot);

        // Dispara a extracao SO quando sujo. A geometria emissiva do projeto e estatica, entao o
        // caso comum e nao fazer nada — ao contrario do RTXDI, que reconstroi tudo todo frame.
        void Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap);

        // Marcar sujo quando transform, material emissivo ou o conjunto de renderables mudar.
        void MarkDirty() { Dirty = true; }

        bool IsReady()          const { return Ready; }
        u32  LightSRVSlot()     const { return LightsSRV; }
        u32  LightCount()       const { return NumTriangles; }

        void Release(FTextureSRVHeap& SRVHeap);

    private:
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        FStats SceneStats{};

        FVolumetricPipeline ExtractPSO;   // 2 SRV, 1 UAV, bindless
        bool Initialized = false;
        bool Ready       = false;
        bool Dirty       = false;

        u32 NumTasks     = 0;
        u32 NumTriangles = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> TaskBuffer;   // upload; escrito uma vez por cena
        Microsoft::WRL::ComPtr<ID3D12Resource> LightBuffer;  // default; saida da extracao
        Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
        u8* MappedCB = nullptr;

        D3D12_RESOURCE_STATES LightState = D3D12_RESOURCE_STATE_COMMON;

        u32 TaskSRV   = 0xFFFFFFFFu;
        u32 LightsSRV = 0xFFFFFFFFu;
        u32 LightsUAV = 0xFFFFFFFFu;
        u32 ExtractTable = 0xFFFFFFFFu; // t0 = tasks, t1 = instances
    };
}
