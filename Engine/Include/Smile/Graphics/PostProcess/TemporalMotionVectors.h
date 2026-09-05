#pragma once

#include "Smile/Scene/CapturedIdentity.h"

#include "Smile/Core/Types.h"
#include "Smile/Math/Math.h"
#include "Smile/Graphics/Backend/D3D12/CommandQueue.h"
#include "Smile/Graphics/Backend/D3D12/GpuResources.h"
#include "Smile/Graphics/Backend/D3D12/ComputePipeline.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace Smile {
    class FScene;
    class FGpuProfiler;
    class FTextureSRVHeap;
    class FGpuSceneInstanceTable;

    // Dados compartilhados pelos vetores temporais especificos do cap. 25 de Ray Tracing Gems II.
    // O buffer e indexado pelo InstanceID da TLAS (que, na Smile, e o indice do FRenderable).
    struct FTemporalInstanceTransformGPU {
        Mat44 CurrentToPrevious;
        Mat44 PreviousToCurrent;
    };
    static_assert(sizeof(FTemporalInstanceTransformGPU) == 128);

    // Contadores que o shader compara com indices. Ficam em u32 porque sao CAPACIDADES, e
    // capacidade nao herda o teto de 24 bits do id.


    struct alignas(256) FTemporalMotionConstants {
        Mat44 InvViewProj;
        Mat44 ViewProj;
        Mat44 PrevViewProj;
        Vec4  CameraPos;    // xyz = camera atual
        Vec4  ScreenParams; // width, height, 1/width, 1/height
        Vec4  Params;       // x=frame slot, y=history valid, z=position reject scale, w=instance count
        // A superficie ANTERIOR tem de ser validada contra a contagem ANTERIOR: um id gravado
        // quando a cena tinha mais objetos passa na contagem de agora sem ser valido.
        //
        // INTEIRO, e nao Vec4. O teto de 2^24 limita o ID de instancia, nao a capacidade do
        // pool de slots -- e a folga do SceneCapacityFor ja a leva acima disso: com 16.000.001
        // renderables a capacidade e 17.000.001, que em float vira 17.000.000 e REJEITA um slot
        // valido; com 16.000.003 ela vira 17.000.004 e deixa passar um indice ALEM do remap.
        FTemporalIdentityParams IdentityParams;
    };
    static_assert(sizeof(FTemporalMotionConstants) == 256);

    // Constrói duas peças de infraestrutura usadas pelos tres casos do capitulo:
    //  1. um historico de superficie (world position + InstanceID) por pixel;
    //  2. um motion vector dual, usado apenas pelos acumuladores de iluminacao.
    //
    // O velocity raster continua sendo a fonte do TAA/DLSS. Reutilizar cor com o vetor dual gera
    // o padrao repetitivo discutido na Fig. 25-5b; ReSTIR/NRD, por outro lado, reutilizam amostras
    // ou radiancia e sao os consumidores corretos.
    class FTemporalMotionVectors : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Motion temporal confiavel"; }
        bool IsInitialized() const override { return Ready; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;
        EHistoryTarget HistoryTargets() const override { return EHistoryTarget::TemporalMotion; }
        void OnInvalidateHistory(EHistoryTarget) override { InvalidateHistory(); }

        void Initialize(ID3D12Device* Device);

        void SetupForScene(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                           const FScene& Scene, u32 TlasSlot, u32 InstanceGeoSlot);
        void SetupForResize(ID3D12Device* Device, FTextureSRVHeap& SRVHeap,
                            u32 Width, u32 Height, u32 DepthSlot, u32 VelocitySlot);

        // Sobe a tabela de identidades do frame para a copia em DEFAULT.
        //
        // Separado do UpdatePerFrame porque precisa de COMMAND LIST: o UpdatePerFrame so
        // escreve em memoria mapeada, e esta copia tem de ficar ordenada na fila direta,
        // antes de qualquer consumidor.
        void RecordIdentityUpload(ID3D12GraphicsCommandList* CommandList, u32 FrameSlot);

        // Slot de SRV da tabela do frame pedido. O consumidor do frame ATUAL le a tabela
        // ANTERIOR para resolver ids gravados la -- e por isso que sao duas, e por isso que
        // elas nao podem viver num UPLOAD de dois slices (ver o contrato no plano).
        u32 IdentitySRV(u32 FrameSlot) const {
            return IdentitySRVSlot[FrameSlot % kFrames];
        }

        void UpdatePerFrame(u32 FrameSlot, const FScene& Scene,
                            const FGpuSceneInstanceTable& Instances,
                            const Mat44& InvViewProj, const Mat44& ViewProj,
                            const Mat44& PrevViewProj, const Vec3& CameraPos);
        void Record(ID3D12GraphicsCommandList* CL, FTextureSRVHeap& SRVHeap,
                    FGpuProfiler* Profiler = nullptr);

        void InvalidateHistory() { HistoryValid = false; }
        bool HasHistory() const  { return HistoryValid; }
        bool IsReady() const     { return Ready; }

        u32 MotionSRV() const { return ReliableMotionSRV; }
        u32 TransformSRV(u32 Slot) const {
            return TransformSRVSlot[Slot % FCommandQueue::kFramesInFlight];
        }
        u32 SurfaceSRV(u32 Slot) const {
            return SurfaceSRVSlot[Slot % FCommandQueue::kFramesInFlight];
        }
        u32 InstanceCount() const { return InstanceCount_; }
        u32 RemapSRV() const { return SlotRemapSRVSlot; }
        FTemporalIdentityParams IdentityParams() const { return CPU.IdentityParams; }
        // Capacidade do remap. O renderer compara com a capacidade do POOL para decidir
        // re-setup: os slots crescem por churn, sem a contagem viva mudar.
        u32 SlotCapacity() const { return SlotCapacity_; }

    private:
        void CreatePipelines(ID3D12Device* Device); // Initialize e OnRecreatePipelines
        void ReleaseResize(FTextureSRVHeap& SRVHeap);
        void ReleaseScene(FTextureSRVHeap& SRVHeap);
        void Transition(ID3D12GraphicsCommandList* CL, ID3D12Resource* Resource,
                        D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After);
        D3D12_GPU_VIRTUAL_ADDRESS CBAddr() const;

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        static constexpr u32 kFrames = FCommandQueue::kFramesInFlight;
        static_assert(kFrames == 2, "surface history assume ping-pong de dois frames");

        FComputePipeline SurfacePSO; // depth + TLAS + InstanceGeo -> surface history
        FComputePipeline DualPSO;    // raster MV + surface histories + transforms -> reliable MV

        Microsoft::WRL::ComPtr<ID3D12Resource> CB;
        u8* MappedCB = nullptr;
        FTemporalMotionConstants CPU{};
        u32 FrameSlot = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> TransformBuffer;
        u8* MappedTransforms = nullptr;
        u32 TransformSRVSlot[kFrames] = { kInvalidSlot, kInvalidSlot };
        // Tabela de identidades: id de instancia -> {slot, generation}.
        //
        // DEFAULT em ping-pong, e nao UPLOAD com dois slices. A tabela de F e lida por F+1, e o
        // BeginFrame de F+2 so espera a fence de F -- entao sobrescrever o UPLOAD ali seria
        // corrida com leituras de F+1 ainda em voo. Com DEFAULT, a CPU nunca toca a memoria que
        // os shaders consultam: ela escreve no staging e a GPU copia, ordenada na fila direta.
        ComPtr<ID3D12Resource> IdentityTable[kFrames];
        GpuResources::FUploadBuffer IdentityStaging;
        u32                    IdentitySRVSlot[kFrames] = { kInvalidSlot, kInvalidSlot };
        // Estado de cada tabela, para a transicao da copia ser correta na primeira vez e nas
        // seguintes sem precisar de barreira desnecessaria.
        D3D12_RESOURCE_STATES  IdentityState[kFrames] = {
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST
        };
        // Remap slot -> indice na lista ATUAL. Fecha a cadeia: a tabela anterior diz QUEM era,
        // e o remap diz ONDE ele esta agora, porque InstanceTransforms continua indexado pela
        // lista.
        //
        // Um so buffer, e nao ping-pong como a tabela: o remap e lido apenas pelo frame que o
        // escreveu, entao nao precisa sobreviver a F+1. A tabela precisa -- e dai a assimetria.
        // A copia sai ordenada na fila direta depois das leituras do frame anterior.
        ComPtr<ID3D12Resource>      SlotRemap;
        GpuResources::FUploadBuffer SlotRemapStaging;
        u32                         SlotRemapSRVSlot = kInvalidSlot;
        D3D12_RESOURCE_STATES       SlotRemapState   = D3D12_RESOURCE_STATE_COPY_DEST;
        u32                         SlotCapacity_    = 0;
        // Contagem do frame anterior, para validar a superficie anterior contra ela.
        u32 PreviousCount_ = 0;
        u32 InstanceCount_ = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource> Surface[kFrames];
        D3D12_RESOURCE_STATES SurfaceState[kFrames] = {
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        u32 SurfaceSRVSlot[kFrames] = { kInvalidSlot, kInvalidSlot };
        u32 SurfaceUAVSlot[kFrames] = { kInvalidSlot, kInvalidSlot };
        u32 SurfaceTable[kFrames]   = { kInvalidSlot, kInvalidSlot };

        Microsoft::WRL::ComPtr<ID3D12Resource> ReliableMotion;
        D3D12_RESOURCE_STATES ReliableMotionState = D3D12_RESOURCE_STATE_COMMON;
        u32 ReliableMotionSRV = kInvalidSlot;
        u32 ReliableMotionUAV = kInvalidSlot;
        u32 DualTable[kFrames] = { kInvalidSlot, kInvalidSlot };

        u32 TlasSRVSlot = kInvalidSlot;
        u32 InstanceGeoSRVSlot = kInvalidSlot;
        u32 Width = 0, Height = 0;
        bool Initialized = false;
        bool SceneReady = false;
        bool Ready = false;
        bool HistoryValid = false;
        f32 PositionRejectScale = 0.02f;
    };
}
