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
        DDGI = 0,   // irradiancia do volume, quando ele existe
        Environment, // ambiente hemisferico/IBL — nao existe no cbuffer dos passes de RT hoje
        Black        // zero explicito; util para MEDIR de quanto o fallback e responsavel
    };

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
