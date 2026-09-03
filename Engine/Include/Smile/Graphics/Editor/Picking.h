#pragma once

#include "Smile/Scene/CapturedIdentity.h"

#include <d3d12.h>
#include <cstddef>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/Backend/D3D12/DescriptorHeap.h"
#include "Smile/Graphics/Renderer/RenderPass.h"

namespace Smile {
    class FGpuMesh;
    class FMaterial;
    class FTextureSRVHeap;

    // Picking GPU por ID-buffer (estilo hit-proxy da Unreal), sob demanda e sem stall.
    //
    // O passe re-renderiza a geometria visivel em um ID buffer com depth proprio. Materiais
    // masked repetem o alpha clip e blends ignoram fragmentos praticamente transparentes.
    // O texel solicitado volta por readback sem bloquear a fila grafica.
    class FObjectPicker : public FRenderPass {
    public:
        // --- Contrato de passe (RenderPass.h) ---
        const char* Name() const override { return "Picking (ID pass)"; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        struct FDrawItem {
            const FGpuMesh*           Mesh;
            const FMaterial*          Material;
            D3D12_GPU_VIRTUAL_ADDRESS ObjectCBAddress; // slot b2 desse renderavel (mesmo MVP do forward)
            // Indice na tabela de identidades DESTE passe, + 1 (0 fica reservado para vazio).
            // NAO e o indice do renderavel na lista: o readback chega frames depois, e ate la a
            // lista pode ter mudado -- era assim que o pick endereçava, e so nao trocava de
            // objeto porque toda mudanca estrutural cancelava o pick em voo.
            u32                       ObjectId;
        };

        // Quem era o objeto quando o passe foi GRAVADO. O resultado do pick e lido frames
        // depois, entao a resposta tem de vir daqui e nao da lista de agora.
        //
        // Os dois campos servem a coisas diferentes: o Handle diz se aquela residencia de GPU
        // ainda e a mesma (o objeto pode ter morrido no meio, e o slot ja pertencer a outro), e
        // o Id e a identidade de EDITOR, que resolve para a posicao atual por um mapa O(1) sem
        // depender de a lista nao ter andado.
        using FPickIdentity = FCapturedIdentity;

        // Identidades do passe corrente, na ordem em que os FDrawItem foram montados. O
        // ObjectId de cada item indexa aqui (menos 1).
        std::vector<FPickIdentity>& Identities() { return PickIdentities; }

        void Initialize(ID3D12Device* Device, u32 Width, u32 Height);
        void Resize(ID3D12Device* Device, u32 Width, u32 Height);
        bool IsInitialized() const override { return Initialized; }

        // Editor pede um pick no pixel (x,y) do backbuffer. Ignora se ja ha um pendente.
        void RequestPick(u32 X, u32 Y);
        bool HasPendingRequest() const { return State == EState::Requested; }
        // Requested pode ser removido imediatamente. Copied continua ocupando o readback ate a
        // fence do frame, mas o resultado e descartado assim que ficar seguro.
        void CancelPending() {
            if (State == EState::Requested) {
                State = EState::Idle;
                DiscardResult = false;
            } else if (State == EState::Copied) {
                if (ReadyCountdown <= 0) {
                    State = EState::Idle;
                    ReadyCountdown = -1;
                    DiscardResult = false;
                } else {
                    DiscardResult = true;
                }
            }
        }

        // Grava um depth/ID proprio e copia o texel. Materiais masked repetem o alpha clip;
        // translucidos escrevem apenas neste depth privado para o frontal continuar selecionavel.
        void RecordIDPass(ID3D12GraphicsCommandList* CmdList,
                          FTextureSRVHeap& SRVHeap,
                          const FDrawItem* Items, size_t Count);

        // Avanca o relogio interno (1x por frame, no topo do RenderFrame apos o BeginFrame).
        void Tick();

        // Devolve true (uma unica vez) quando o resultado fica pronto. OutIdentity fica com a
        // identidade gravada no momento do passe; Id == 0 significa clique no vazio.
        //
        // Quem chama TEM de validar o Handle contra o allocator antes de usar o Id: entre o
        // passe e este readback o objeto pode ter sido removido, e ai a resposta certa e
        // 'nada', nao 'o objeto que ocupa aquele lugar agora'.
        bool TryResolve(FPickIdentity& OutIdentity);

    private:
        void CreateTargets(ID3D12Device* Device, u32 Width, u32 Height);
        void BuildRootSignature(ID3D12Device* Device);
        void BuildPSO(ID3D12Device* Device);

        enum class EState { Idle, Requested, Copied };

        ComPtr<ID3D12Resource>      IDTarget;   // R32_UINT 1x1; viewport deslocado ate o cursor
        FDescriptorHeap             IDRTVHeap;  // 1 RTV
        ComPtr<ID3D12Resource>      PickDepth;
        FDescriptorHeap             PickDSVHeap;
        ComPtr<ID3D12Resource>      Readback;   // buffer READBACK (1 texel, row-pitch alinhado)
        ComPtr<ID3D12RootSignature> RootSig;
        ComPtr<ID3D12PipelineState> PSO;
        ComPtr<ID3D12PipelineState> AlphaPSO;

        EState State          = EState::Idle;
        std::vector<FPickIdentity> PickIdentities;

        u32    PickX          = 0;
        u32    PickY          = 0;
        int    ReadyCountdown = -1; // frames ate o readback ficar seguro (>=0 = aguardando)
        u32    TargetWidth    = 0;
        u32    TargetHeight   = 0;
        bool   DiscardResult  = false;
        bool   Initialized    = false;
    };
}
