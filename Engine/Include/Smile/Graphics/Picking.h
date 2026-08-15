#pragma once

#include <d3d12.h>
#include <cstddef>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/DescriptorHeap.h"
#include "Smile/Graphics/RenderPass.h"

namespace Smile {
    class FGpuMesh;

    // Picking GPU por ID-buffer (estilo hit-proxy da Unreal), sob demanda e sem stall.
    //
    // Fluxo: o editor pede um pick num pixel (RequestPick). No frame seguinte, APOS o passe
    // principal (depth da cena ja escrito), o Renderer chama RecordIDPass: re-renderiza a
    // geometria visivel num RT R32_UINT reusando o MVP do ObjectCB (b2) e testando depth-EQUAL
    // contra o depth da cena — so o fragmento da superficie visivel passa, recortando ate a
    // folhagem alpha-masked de graca (os furos das folhas nunca escreveram depth). O ID gravado
    // e (indiceDoRenderavel + 1); 0 = vazio. Em seguida copia o texel sob o cursor p/ um buffer
    // READBACK. Como a copia faz parte do frame F, ela so termina quando a GPU passa a fence de
    // F; com kFramesInFlight=2 isso esta garantido 2 frames depois — TryResolve devolve o ID
    // entao (indice = valor - 1; -1 = nada).
    class FObjectPicker : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Picking (ID pass)"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        struct FDrawItem {
            const FGpuMesh*           Mesh;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCBAddress; // slot b2 desse renderavel (mesmo MVP do forward)
            u32                       ObjectId;        // indice do renderavel + 1 (0 reservado p/ vazio)
        };

        void Initialize(ID3D12Device* Device, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, u32 Width, u32 Height);
        bool IsInitialized() const override { return Initialized; }

        // Editor pede um pick no pixel (x,y) do backbuffer. Ignora se ja ha um pendente.
        void RequestPick(u32 X, u32 Y);
        bool HasPendingRequest() const { return State == EState::Requested; }
        // Descarta a requisicao pendente sem gravar nada (ex.: MSAA ligado). Evita ficar preso.
        void CancelPending() { if (State == EState::Requested) State = EState::Idle; }

        // Grava o passe de ID + a copia do texel. Chamar apos o passe principal, so quando
        // HasPendingRequest(). SceneDSV = DSV (writable) do depth da cena; o PSO testa EQUAL com
        // DepthWriteMask=ZERO, entao o depth pode ficar em DEPTH_WRITE (sem barrier).
        void RecordIDPass(ID3D12GraphicsCommandList* CmdList,
                          D3D12_CPU_DESCRIPTOR_HANDLE SceneDSV,
                          const FDrawItem* Items, size_t Count,
                          u32 Width, u32 Height);

        // Avanca o relogio interno (1x por frame, no topo do RenderFrame apos o BeginFrame).
        void Tick();

        // Devolve true (uma unica vez) quando o resultado fica pronto; OutIndex = indice do
        // renderavel (-1 = vazio / sem hit). False enquanto nao pronto.
        bool TryResolve(int& OutIndex);

    private:
        void CreateTargets(ID3D12Device* Device, u32 Width, u32 Height);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device);

        enum class EState { Idle, Requested, Copied };

        ComPtr<ID3D12Resource>      IDTarget;   // R32_UINT full-res
        FDescriptorHeap             IDRTVHeap;  // 1 RTV
        ComPtr<ID3D12Resource>      Readback;   // buffer READBACK (1 texel, row-pitch alinhado)
        ComPtr<ID3D12RootSignature> RootSig;
        ComPtr<ID3D12PipelineState> PSO;

        EState State          = EState::Idle;
        u32    PickX          = 0;
        u32    PickY          = 0;
        int    ReadyCountdown = -1; // frames ate o readback ficar seguro (>=0 = aguardando)
        u32    TargetWidth    = 0;
        u32    TargetHeight   = 0;
        bool   Initialized    = false;
    };
}
