#pragma once

#include "Smile/Core/Types.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace Smile {
    class FTextureSRVHeap;
    class FCommandQueue;

    // ============================================================================================
    // Fallback indireto — o que ReSTIR GI e reflexoes leem quando o terminador do caminho nao e o
    // cache nem o proprio hit.
    //
    // POR QUE EXISTE COMO CONTRATO, E NAO COMO TRES ARGUMENTOS SOLTOS
    // Ate a Fase 1 da serie SHaRC, `FReSTIRGI::SetupForResize` e `FReflections::SetupForResize`
    // recebiam os tres slots do DDGI na unha, e o Renderer so os chamava depois de um
    // `if (!DDGI.IsReady()) return;`. Isso amarrava DOIS passes de RT que nao consultam sonda
    // nenhuma no caminho principal a existencia de um volume: sem DDGI, nem reflexao nem ReSTIR GI
    // chegavam a IsReady(), e o NRD indireto ia junto. Passar a struct impede que a dependencia
    // volte por descuido — quem quiser um slot novo do DDGI tem de acrescenta-lo AQUI, onde a
    // ausencia do volume ja e um caso previsto.
    //
    // DESCRITOR VALIDO SEMPRE, LEITURA FECHADA POR FLAG
    // `Available == false` NAO significa slot invalido. Tabela de descritores nao aceita buraco:
    // um kInvalidSlot (0xFFFFFFFF) copiado para uma tabela vira base + 0xFFFFFFFF * HandleSize,
    // ~128 GiB fora do heap, e o CopyDescriptors le desse endereco — access violation, ou pior,
    // descritor corrompido em silencio. Entao os slots apontam para recursos NEUTROS (zerados) e
    // quem fecha a leitura e o shader, pelo bit que o FGIHitSampling::SkipModePacked publica.
    // Ler o neutro por engano custaria indireto preto, nao lixo; mas o gate existe para que nem
    // isso aconteca, e para que o custo do tap tambem saia.
    // ⚠️ EXISTENCIA FISICA, NAO HABILITACAO — e a distincao e de TEMPO, nao de gosto.
    // Esta struct e lida uma vez, no setup/resize, e o resultado fica LATCHED dentro das tabelas
    // de descritores dos consumidores. Um knob de habilitacao (o `UseGI` do Renderer) muda a
    // qualquer momento sem realocar nada — pOr isso ele NAO pode participar da escolha feita aqui.
    // Ja foi assim uma vez: com `UseGI` nos dois lados, um setup com a GI desligada gravava os
    // neutros nas tabelas e religar a GI abria o gate do shader (por frame) sobre um atlas 1x1
    // zerado (latched), deixando o terminador preto ate o proximo setup.
    //
    // A divisao que vale: EXISTENCIA manda no descritor (aqui), HABILITACAO manda na leitura
    // (FGIHitSampling::FallbackAvailable, recalculado todo frame). Quem acrescentar campo nesta
    // struct precisa responder antes: "isto pode mudar sem passar por um SetupForResize?" Se puder,
    // nao pertence a esta struct.
    struct FGIFallbackBindings {
        u32  IrradianceAtlasSRV = 0xFFFFFFFFu;
        u32  DistanceAtlasSRV   = 0xFFFFFFFFu;
        u32  ProbeDataSRV       = 0xFFFFFFFFu;
        // O volume DDGI EXISTE e os slots acima sao os dele. false = slots neutros. Em ambos os
        // casos os tres sao validos; isto nao diz se a GI esta ligada neste frame.
        bool Available          = false;
    };

    // Os recursos neutros em si. Vivem no Renderer e nao no FDDGI — sao justamente o que responde
    // quando o FDDGI nao existe.
    //
    // Criados uma vez no Initialize: nao dependem de cena nem de resolucao. 1x1 e 1 elemento
    // porque nada os le; eles existem para o descritor ter destino. O conteudo e zero, que e a
    // resposta correta caso alguma leitura escape do gate — irradiancia zero, momentos de
    // distancia zero, ProbeData zero (sonda na origem, sem offset de relocacao).
    class FGIFallbackResources {
    public:
        // Precisa da fila: o zero e ESCRITO (clear via UAV) e nao presumido. Conteudo de recurso
        // committed nasce indefinido por spec — drivers costumam zerar, mas "costumam" nao e
        // contrato, e um recurso que o codigo chama de neutro tem de ser neutro de fato.
        // Roda uma vez no Initialize do Renderer; nao depende de cena nem de resolucao.
        void Initialize(ID3D12Device* Device, FCommandQueue& Queue, FTextureSRVHeap& SRVHeap);
        void Release(FTextureSRVHeap& SRVHeap);

        bool IsReady() const { return Ready; }
        // Bindings com Available = false. O caller sobrescreve com os slots do DDGI quando ha
        // volume; ver Renderer::GIFallbackBindingsForSetup.
        FGIFallbackBindings Bindings() const;

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> IrradTex;  // 1x1 RGBA16F
        Microsoft::WRL::ComPtr<ID3D12Resource> DistTex;   // 1x1 RG16F
        Microsoft::WRL::ComPtr<ID3D12Resource> ProbeBuf;  // 1 x float4

        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
        u32  IrradSRV     = kInvalidSlot;
        u32  DistSRV      = kInvalidSlot;
        u32  ProbeDataSRV = kInvalidSlot;
        bool Ready        = false;
    };
}
