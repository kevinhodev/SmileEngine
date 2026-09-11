#pragma once

#include <d3d12.h>
#include "Smile/Core/Types.h"
#include "Smile/Graphics/Renderer/RenderPass.h"
#include "Smile/Graphics/Scene/GpuIndirectDraw.h"

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

        // Indice do parametro de root do bloco de constantes de draw (b4). Publico porque quem
        // escreve por draw e o passe, nao esta classe.
        //
        // O bloco nasceu na Fase 3 com as duas primeiras (material) e a Fase 4 acrescentou as tres
        // seguintes. O nome continua `kMaterialRootIndex` porque ele aparece em call sites de tres
        // subsistemas; o que descreve o conteudo sao os offsets abaixo.
        static constexpr u32 kMaterialRootIndex = 14;
        // CBV b2, visibilidade VERTEX. Carrega o `ObjectConstants` do item no caminho classico e
        // as tres matrizes de view do passe no bindless — o mesmo parametro, porque o que ele
        // tinha de dependente do objeto foi para a tabela canonica. Era um `4` literal em oito
        // call sites de submissao.
        static constexpr u32 kObjectCBRootIndex = 4;
        // Offsets DENTRO do bloco. Nomeados porque um inteiro cru no call site nao diz qual
        // constante esta sendo escrita, e trocar dois deles compila e desenha.
        //
        // Quatro sao por PASSE e uma e por DRAW: `MaterialRow` muda por grupo de material,
        // `InstanceSlot` muda por item. As outras tres sao descritores publicados uma vez.
        //
        // ALIAS: os valores moram no contrato compartilhado (Scene/GpuIndirectDraw.h) desde a Fase
        // 5, porque a command signature os usa como `DestOffsetIn32BitValues` e o teste que executa
        // o comando de verdade precisa dos MESMOS numeros sem linkar a engine. Os nomes daqui
        // ficaram: eles aparecem em call sites de tres subsistemas.
        static constexpr u32 kRootConstMaterialTable  = GpuDrawConstants::kMaterialTable;
        static constexpr u32 kRootConstMaterialRow    = GpuDrawConstants::kMaterialRow;
        static constexpr u32 kRootConstTransformTable = GpuDrawConstants::kTransformTable;
        static constexpr u32 kRootConstPrevTransform  = GpuDrawConstants::kPrevTransform;
        static constexpr u32 kRootConstInstanceSlot   = GpuDrawConstants::kInstanceSlot;
        static constexpr u32 kRootConstCount          = GpuDrawConstants::kCount;

        // A command signature do main view, criada JUNTO da root signature a que ela esta amarrada
        // — `CreateCommandSignature` exige a root signature quando o comando escreve constantes, e
        // no ExecuteIndirect a que estiver ligada tem de ser a MESMA. Morando as duas nesta classe,
        // no mesmo Initialize, elas nao tem como divergir.
        //
        // Uma so basta para o main view inteiro: as 22 PSOs de raster saem do mesmo
        // `PSODesc.pRootSignature`. Sombras, chuva e picking tem root signatures proprias e
        // ganharao as suas quando forem migradas — compartilhando a struct, nao o objeto.
        ID3D12CommandSignature* IndirectDrawSignature() const {
            return IndirectDrawCommandSignature.Get();
        }

        // "As PSOs de transform bindless existem?" — mesma pergunta que BindlessMaterialsReady,
        // pelo mesmo motivo: capability ligada com .cso ausente daria PSO nula.
        //
        // ⚠️ As quatro variantes com material sao construidas SOBRE as bindless de material, e nao
        // em paralelo a elas. Isso e acoplamento deliberado: com os dois eixos independentes
        // seriam quatro combinacoes de PSO por passe, num arquivo que nomeia cada PSO a mao. A
        // escada do A/B fica classico -> material bindless -> material + transform bindless, e
        // cada degrau isola uma mudanca. As duas de profundidade OPACA nao tem material e por isso
        // sao independentes.
        //
        // A lista tem de cobrir TODAS as PSOs que o toggle passa a usar, e nao so as do caminho
        // principal: quem responde `true` aqui autoriza a LIBERACAO do ObjectCB, e um passe cuja
        // gemea VS-bindless faltou cairia no classico — que deixou de existir. A do ForwardBlend
        // entrou por esse motivo, depois de ficar de fora e nao ser notada.
        bool BindlessTransformsReady() const {
            return BindlessMaterialsReady() && PipelineStateDepthOnlyVsBindless &&
                   PipelineStateDepthNormalVsBindless && PipelineStateGBufferVsBindless &&
                   PipelineStateGBufferTwoSidedVsBindless &&
                   PipelineStateDepthOnlyMaskedVsBindless &&
                   PipelineStateDepthNormalMaskedVsBindless &&
                   PipelineStateForwardBlendVsBindless;
        }
        ID3D12PipelineState* PSODepthOnlyVsBindless() const {
            return PipelineStateDepthOnlyVsBindless.Get();
        }
        ID3D12PipelineState* PSODepthNormalVsBindless() const {
            return PipelineStateDepthNormalVsBindless.Get();
        }
        ID3D12PipelineState* PSOGBufferVsBindless() const {
            return PipelineStateGBufferVsBindless.Get();
        }
        ID3D12PipelineState* PSOGBufferTwoSidedVsBindless() const {
            return PipelineStateGBufferTwoSidedVsBindless.Get();
        }
        ID3D12PipelineState* PSODepthOnlyMaskedVsBindless() const {
            return PipelineStateDepthOnlyMaskedVsBindless.Get();
        }
        ID3D12PipelineState* PSODepthNormalMaskedVsBindless() const {
            return PipelineStateDepthNormalMaskedVsBindless.Get();
        }
        // Deferred lighting fullscreen (le o G-buffer -> HDR), na root signature principal.
        // Aditivo: soma a luz sobre o emissivo que o geometry pass escreveu no SceneColor.
        // Debug: variante opaca p/ as views de SSAO/GI (o shader substitui a tela inteira).
        ID3D12PipelineState* PSODeferredLighting()      const { return PipelineStateDeferredLighting.Get(); }
        ID3D12PipelineState* PSODeferredLightingDebug() const { return PipelineStateDeferredLightingDebug.Get(); }
        // Forward de translucidos (materiais Blend): alpha-blend premultiplicado sobre o HDR,
        // depth read-only, cull none (vidro two-sided).
        ID3D12PipelineState* PSOForwardBlend() const { return PipelineStateForwardBlend.Get(); }
        ID3D12PipelineState* PSOForwardBlendVsBindless() const {
            return PipelineStateForwardBlendVsBindless.Get();
        }

    private:
        ComPtr<ID3D12RootSignature> RootSignature;
        ComPtr<ID3D12CommandSignature> IndirectDrawCommandSignature;
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

        // Fase 4: as mesmas seis com o VERTEX shader lendo o transform da tabela canonica. As duas
        // primeiras nao tem pixel shader de material e por isso nao dependem do eixo da Fase 3.
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnlyVsBindless;
        ComPtr<ID3D12PipelineState> PipelineStateDepthNormalVsBindless;
        ComPtr<ID3D12PipelineState> PipelineStateGBufferVsBindless;
        ComPtr<ID3D12PipelineState> PipelineStateGBufferTwoSidedVsBindless;
        ComPtr<ID3D12PipelineState> PipelineStateDepthOnlyMaskedVsBindless;
        ComPtr<ID3D12PipelineState> PipelineStateDepthNormalMaskedVsBindless;
        bool                        SupportsBindlessMaterials = false;
        ComPtr<ID3D12PipelineState> PipelineStateDeferredLighting;
        ComPtr<ID3D12PipelineState> PipelineStateDeferredLightingDebug;
        ComPtr<ID3D12PipelineState> PipelineStateForwardBlend;
        ComPtr<ID3D12PipelineState> PipelineStateForwardBlendVsBindless;
    };
} 
