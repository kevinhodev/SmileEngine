#pragma once

#include "Smile/Core/Types.h"

// POLITICA DO INDIRETO — quem produz o sinal, e quem responde quando ele nao consegue.
//
// Este header existe por causa de uma confusao MEDIDA, e nao por gosto de taxonomia. Ate a Fase 6
// havia um unico `UseGI`, e a auditoria de abertura encontrou `UseGI && DDGI.IsReady()` repetido em
// oito pontos do Renderer servindo TRES perguntas diferentes:
//
//   1. o passe do DDGI roda neste frame?          (grid, trace, update das sondas)
//   2. o DDGI responde o miss do raio secundario? (FGIHitSampling::FallbackAvailable)
//   3. o deferred e a nevoa leem o atlas?         (GITable, VolumetricFog)
//
// Sao independentes. A (3) e consumo VOLUMETRICO e sobrevive a qualquer decisao sobre GI de
// superficie — fog precisa de irradiancia volumetrica e nao tem substituto no cache, que e
// direcionalmente cego e esparso. A (2) e politica de FALLBACK. A (1) e orcamento.
//
// O custo da confusao nao foi teorico: durante a Fase 5 um gate foi escrito assumindo que `UseGI`
// governava a execucao do ReSTIR GI. Nao governa — `ReSTIRGIActive = UseReSTIRGI &&
// ReSTIRGI.IsReady()` —, e o resultado foi um botao inerte na UI. O nome mentia, e alguem
// acreditou.
namespace Smile {

    // QUEM PRODUZ o indireto de superficie. Nao e escala de qualidade: sao estimadores
    // diferentes, com historicos diferentes, e trocar entre eles invalida acumulador.
    enum class EIndirectPrimary : u32 {
        // ReSTIR GI com o radiance cache como terminador primario dos hits elegiveis. E o default
        // desde a Fase 5, e o que a serie inteira mediu.
        ReSTIR_SHaRC = 0,
        // O volume DDGI como sinal principal de superficie. Continua alcancavel de proposito: e o
        // ROLLBACK da serie, e o controle de A/B contra o qual as baselines da Fase 0 foram
        // tiradas. Nao remover sem um gate que prove que ninguem precisa mais dele.
        DDGI,
        // Sem indireto de superficie. O direto continua inteiro; a nevoa continua lendo o atlas se
        // o volume existir (ver a nota (3) acima).
        Off
    };

    // QUEM RESPONDE quando o primario nao consegue — miss do cache, celula fria, ou geometria que
    // o cache nao pode representar (segmento curto, cone estreito).
    //
    // A escolha aqui NAO e cosmetica, e a Fase 5 mediu o tamanho dela: o fallback responde 30,14%
    // dos hits secundarios na Bistro exterior. Trocar DDGI por Black nao apaga 30% do brilho —
    // apaga 30% dos CAMINHOS, e o que sobra e uma imagem mais escura de forma desigual.
    enum class EIndirectFallback : u32 {
        DDGI = 0,    // irradiancia do volume, quando ele existe
        // ⚠️ DECLARADO, NAO IMPLEMENTADO. Nao ha cor de ambiente no cbuffer de um passe de RT, e o
        // gather desvanece para preto fora do volume justamente por isso. O
        // `Renderer::EffectiveFallback` DEGRADA este valor para Black enquanto for assim — pedi-lo
        // e pedir Black, e reportar "environment" no manifesto seria publicar um estado que o
        // shader nao produz. Ele fica no enum como intencao nomeada, e nao como pendencia
        // escondida atras de um numero que ninguem honra.
        Environment,
        Black        // zero explicito; util para MEDIR de quanto o fallback e responsavel
    };

    // ============================================================================================
    // DOIS CONTRATOS para quem consumir estes enums. Nenhum dos dois e opcional, e os dois saem de
    // defeito ja pago nesta serie.
    //
    // 1. O MANIFESTO GRAVA OS DOIS: pedido E efetivo.
    //
    //    Gravar so o efetivo nao mente sobre a imagem — e essa era a tentacao —, mas apaga a
    //    DEGRADACAO. Uma captura que diz `fallback: black` nao distingue "o operador pediu preto
    //    para medir" de "pediu DDGI e nao havia volume" nem de "pediu Environment, que nao
    //    existe". Sao tres configuracoes diferentes com o mesmo pixel, e a serie inteira mostrou
    //    que duas capturas indistinguiveis na pasta sao um convite a compara-las.
    //
    //    Forma sugerida: `indirectFallbackRequested` + `indirectFallbackEffective`. Iguais na
    //    maioria dos frames; quando divergem, e a divergencia que interessa.
    //
    // 2. MUDANCA DO EFETIVO INVALIDA OS CONSUMIDORES — inclusive a que ninguem pediu.
    //
    //    ⚠️ A armadilha esta aqui: o efetivo muda SEM passar por setter nenhum. Basta o volume
    //    aparecer ou sumir (carga de cena, resize que recria o DDGI, `UseGI` mexido noutro
    //    caminho) para `EffectiveFallback` ir de DDGI a Black e voltar — e isso troca o terminador
    //    do raio secundario para todo mundo, que e exatamente o evento que obriga ReSTIR GI, atlas
    //    do DDGI e NRD a esquecer.
    //
    //    Logo NAO basta invalidar no setter do enum: e preciso um DETECTOR DE BORDA sobre o valor
    //    efetivo, avaliado por frame, no topo — mesmo desenho do aquecimento do radiance cache
    //    (latch + consumidor no Renderer + invalidacao dos consumidores). Aquele levou quatro
    //    rodadas de revisao para ficar certo; este comeca sabendo onde as pedras estao:
    //    o detector roda antes de qualquer consumidor publicar cbuffer, e o latch descreve a
    //    mudanca EFETIVA e nao a transicao de estado.
    //
    //    2a. O PRIMEIRO valor observado apenas INICIALIZA. `uninitialized -> DDGI` nao e borda:
    //        nada mudou, so passou a existir observador. Tratado como borda, ele invalidaria
    //        historico no primeiro frame de toda cena e — pior — cancelaria a sessao de captura
    //        recem-aberta, porque a invalidacao passa pelo funil. O estado anterior nasce
    //        indefinido e a primeira leitura o preenche em silencio.
    //
    //    2b. A MASCARA depende de POR QUE o efetivo mudou, e nao so de que mudou. As tres
    //        perguntas do topo deste arquivo voltam aqui:
    //
    //          - trocou a POLITICA com o volume ainda vivo (ex.: DDGI -> Black): muda o terminador
    //            do raio secundario, entao ReSTIR GI, reflexoes e NRD esquecem. A NEVOA NAO — ela
    //            le o atlas direto, pela pergunta (3), e continua lendo exatamente o mesmo.
    //            Derrubar o historico dela seria custo puro e um flicker sem causa.
    //          - sumiu ou apareceu o VOLUME: ai a nevoa entra junto, porque a fonte dela mudou de
    //            verdade.
    //
    //        Ou seja: uma mascara "consumidores de SUPERFICIE" e outra com os volumetricos. O
    //        `RadianceCacheConsumersOnly` do FRenderSettings inclui VolumetricFog hoje e nao serve
    //        para o primeiro caso.
    // ============================================================================================

    inline const char* IndirectPrimaryName(EIndirectPrimary P) {
        switch (P) {
            case EIndirectPrimary::ReSTIR_SHaRC: return "restir_sharc";
            case EIndirectPrimary::DDGI:         return "ddgi";
            default:                             return "off";
        }
    }
    inline const char* IndirectFallbackName(EIndirectFallback F) {
        switch (F) {
            case EIndirectFallback::DDGI:        return "ddgi";
            case EIndirectFallback::Environment: return "environment";
            default:                             return "black";
        }
    }
}
