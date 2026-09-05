#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/Renderer/RenderPass.h"

namespace Smile {
    class FTextureSRVHeap;

    class FPipelineState : public FPipelineOwner {
    public:
        // --- Dono de pipeline (RenderPass.h): tem shader, mas NAO grava frame ---
        const char* Name() const override { return "Pipeline principal (raster)"; }
        bool IsInitialized() const override { return RootSignature != nullptr; }
        FPassShaderStems ShaderStems() const override;
        void OnRecreatePipelines(const FPassInitContext& Ctx) override;

        // `DirectHeapIndexing` vem do device (FD3D12Device::SupportsDirectHeapIndexing) e decide
        // duas coisas: se a root signature recebe CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED e se as PSOs
        // bindless chegam a existir. Sem ele o raster fica no caminho classico inteiro.
        void Initialize(ID3D12Device* Device, bool DirectHeapIndexing);
        void RecreatePSO(ID3D12Device* Device);

        ID3D12RootSignature* GetRootSignature() const { return RootSignature.Get(); }

        // Amarra o heap de descritores e a root signature do raster, NESTA ORDEM.
        //
        // A ordem e contrato da spec de HLSL Dynamic Resources, e nao estilo:
        //
        //   "SetDescriptorHeaps must be called, passing the corresponding heaps, before a call to
        //    SetGraphicsRootSignature or SetComputeRootSignature that uses either
        //    CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED or SAMPLER_HEAP_DIRECTLY_INDEXED flags."
        //
        // Hoje o raster satisfaz isso por ACIDENTE: o heap e amarrado uma vez no
        // BeginSceneRecording e reamarrado depois de cada corte de segmento, e os nove
        // SetGraphicsRootSignature dos passes caem todos depois de um desses pontos. Nada
        // impede que o proximo corte de segmento — ou um passe novo — inverta a ordem, e o
        // sintoma nao seria erro de API: seria `ResourceDescriptorHeap` lendo de um heap que a
        // GPU nao tem amarrado.
        //
        // Reamarrar o MESMO ponteiro de heap e explicitamente permitido (a proibicao da spec e
        // chamar SetDescriptorHeaps com ponteiros DIFERENTES depois da root signature e antes do
        // draw), e sao nove chamadas por frame.
        void BindGraphicsRoot(ID3D12GraphicsCommandList* Cmd, FTextureSRVHeap& Heap) const;
        ID3D12PipelineState* PSODepthOnly()  const { return PipelineStateDepthOnly.Get(); }
        ID3D12PipelineState* PSODepthNormal() const { return PipelineStateDepthNormal.Get(); }
        // Variantes masked/two-sided do prepass (cull none + PS com alpha-clip): folhagem e
        // grades entram no depth/normal do prepass, entao o GTAO as ve (fim dos speckles).
        ID3D12PipelineState* PSODepthOnlyMasked()   const { return PipelineStateDepthOnlyMasked.Get(); }
        ID3D12PipelineState* PSODepthNormalMasked() const { return PipelineStateDepthNormalMasked.Get(); }
        // Geometry pass do deferred: escreve o G-buffer (MRT 3 RTs). Depth EQUAL (le o depth ja
        // estabelecido). TwoSided = cull none p/ folhagem/alpha-test.
        ID3D12PipelineState* PSOGBuffer()         const { return PipelineStateGBuffer.Get(); }
        ID3D12PipelineState* PSOGBufferTwoSided() const { return PipelineStateGBufferTwoSided.Get(); }

        // --- Material BINDLESS (Fase 3) -------------------------------------------------------
        //
        // Mesmas PSOs, mesmo vertex shader, mesmo estado: so o pixel shader troca pela permutacao
        // que le a linha canonica do material pelo heap em vez do CBV + tabela amarrados por
        // draw. As duas familias coexistem no MESMO executavel porque a regra 2 do plano exige o
        // A/B enquanto o fallback existir — e porque atribuir uma diferenca visual ao "novo
        // acesso a dados" exige poder alternar sem trocar de build.
        //
        // Todas devolvem nullptr quando o device nao tem Resource Binding Tier 3.
        bool SupportsBindless() const { return SupportsBindlessMaterials; }
        // "As quatro PSOs existem?" — a pergunta que o passe faz antes de escolher o caminho.
        // Conferir a capability sozinha nao basta: um .cso ausente daria PSO nula com a
        // capability ligada.
        bool BindlessMaterialsReady() const {
            return SupportsBindlessMaterials && PipelineStateGBufferBindless &&
                   PipelineStateGBufferTwoSidedBindless && PipelineStateDepthOnlyMaskedBindless &&
                   PipelineStateDepthNormalMaskedBindless;
        }
        ID3D12PipelineState* PSOGBufferBindless() const {
            return PipelineStateGBufferBindless.Get();
        }
        ID3D12PipelineState* PSOGBufferTwoSidedBindless() const {
            return PipelineStateGBufferTwoSidedBindless.Get();
        }
        ID3D12PipelineState* PSODepthOnlyMaskedBindless() const {
            return PipelineStateDepthOnlyMaskedBindless.Get();
        }
        ID3D12PipelineState* PSODepthNormalMaskedBindless() const {
            return PipelineStateDepthNormalMaskedBindless.Get();
        }

        // Indice do parametro de root das duas constantes de material (b4): [0] descritor da
        // tabela, [1] linha. Publico porque quem escreve por draw e o passe, nao esta classe.
        static constexpr u32 kMaterialRootIndex = 14;
        // Deferred lighting fullscreen (le o G-buffer -> HDR), na root signature principal.
        // Aditivo: soma a luz sobre o emissivo que o geometry pass escreveu no SceneColor.
        // Debug: variante opaca p/ as views de SSAO/GI (o shader substitui a tela inteira).
        ID3D12PipelineState* PSODeferredLighting()      const { return PipelineStateDeferredLighting.Get(); }
        ID3D12PipelineState* PSODeferredLightingDebug() const { return PipelineStateDeferredLightingDebug.Get(); }
        // Forward de translucidos (materiais Blend): alpha-blend premultiplicado sobre o HDR,
        // depth read-only, cull none (vidro two-sided).
        ID3D12PipelineState* PSOForwardBlend() const { return PipelineStateForwardBlend.Get(); }

    private:
        ComPtr<ID3D12RootSignature> RootSignature;
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnly;
        ComPtr<ID3D12PipelineState> PipelineStateDepthNormal;
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnlyMasked;
        ComPtr<ID3D12PipelineState> PipelineStateDepthNormalMasked;
        ComPtr<ID3D12PipelineState> PipelineStateGBuffer;
        ComPtr<ID3D12PipelineState> PipelineStateGBufferTwoSided;
        ComPtr<ID3D12PipelineState> PipelineStateGBufferBindless;
        ComPtr<ID3D12PipelineState> PipelineStateGBufferTwoSidedBindless;
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnlyMaskedBindless;
        ComPtr<ID3D12PipelineState> PipelineStateDepthNormalMaskedBindless;
        bool                        SupportsBindlessMaterials = false;
        ComPtr<ID3D12PipelineState> PipelineStateDeferredLighting;
        ComPtr<ID3D12PipelineState> PipelineStateDeferredLightingDebug;
        ComPtr<ID3D12PipelineState> PipelineStateForwardBlend;
    };
} 
