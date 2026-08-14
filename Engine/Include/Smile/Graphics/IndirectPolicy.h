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

    // ============================================================================================
    // 3. CADA CALL SITE TERMINA NUMA PERGUNTA NOMEADA. Sao quatro, e nenhuma e sinonimo de outra:
    //
    //      Renderer::DDGIVolumeLive()            -> executar/manter o volume    (orcamento)
    //      Renderer::EffectiveFallback() == DDGI -> fallback dos RAIOS           (politica)
    //      Renderer::DDGISurfaceAvailable()      -> deferred, folhagem/subsurface,
    //                                               translucidos e debug de GI   (consumo)
    //      Renderer::DDGIVolumetricAvailable()   -> SOMENTE nevoa/volume         (consumo)
    //      Renderer::EffectivePrimary()          -> roteamento principal         (estimador)
    //
    //    Duas delas devolvem o MESMO booleano hoje (`VolumeLive` e `VolumetricAvailable`), e isso
    //    nao e duplicacao a eliminar: e o contrato. O dia em que a politica de superficie disser
    //    Black com o volume vivo, a nevoa continua lendo — e nenhum call site volumetrico precisa
    //    mudar, porque ele ja pergunta a coisa certa. Colapsar as duas por serem iguais AGORA
    //    reintroduz exatamente a confusao que custou esta fase.
    //
    //    Criterio de revisao para a classificacao: se um `UseGI` sobreviver, ou se alguem escrever
    //    `DDGIVolumeLive()` num ponto volumetrico, a classificacao falhou mesmo com a imagem
    //    identica.
    //
    //    CLASSIFICACAO FEITA (Renderer.cpp; sao NOVE, e nao sete — a auditoria de abertura contou
    //    por baixo). Uma ja convertida, oito pendentes de substituicao mecanica:
    //
    //      EXECUCAO DO VOLUME  -> DDGIVolumeLive()
    //        HasReGIRConsumer      o trace do DDGI consome ReGIR? (orcamento do pool)
    //        DDGIWillTrace         o passe vai tracar neste frame
    //        bloco do GIComputeFence  bifurcacao para a fila compute
    //
    //      CONSUMO DE SUPERFICIE -> DDGISurfaceAvailable()
    //        CB do frame           DDGIGridMin/Count/Params/DistParams — lidos pelo DEFERRED e
    //                              pelo ForwardBlend
    //        GITable do deferred   GI primaria, fill de folhagem, termo traseiro de subsurface
    //        GITable dos TRANSLUCIDOS  ambiente difuso do ForwardBlend (nao e o debug view!)
    //        DeferredDebugView     a visualizacao de GI, que e de superficie
    //
    //      CONSUMO VOLUMETRICO -> DDGIVolumetricAvailable()
    //        CB da nevoa           VF.* com grid e atlas
    //        VolumetricFog::Execute  SRV do atlas de irradiancia
    //
    //    ⚠️ A PRIMEIRA VERSAO DESTA CLASSIFICACAO ERROU AQUI, e o erro merece ficar: ela pos os
    //    quatro pontos de superficie em "volumetrico" porque todos LEEM O ATLAS. Ler o atlas nao e
    //    a categoria — o USO e. A nevoa integra meio participante; o deferred sombreia superficie,
    //    e os dois respondem a politicas diferentes. Com `primario = Off`, um helper chamado
    //    "volumetrico" continuaria iluminando superficie: imagem identica hoje, mentira no dia da
    //    divergencia. Foi o segundo ponto do arquivo em que "todos devolvem o mesmo booleano"
    //    quase apagou uma distincao real.
    //
    //      POLITICA DE FALLBACK -> EffectiveFallback() == DDGI
    //        GIHit.FallbackAvailable   ✅ ja convertido
    //
    //    Nenhum ponto pediu `EffectivePrimary()`: a escolha da saida principal ainda nao existe
    //    como roteamento — e o commit seguinte, o primeiro que muda imagem. Que a pergunta nao
    //    tenha call site AGORA e informacao, nao lacuna.
    //
    // ============================================================================================
    // 4. O TERCEIRO PAPEL DO DDGI: AUXILIAR DE SUPERFICIE.
    //
    //    A taxonomia tinha dois papeis para o atlas em superficie — primario e fallback dos raios
    //    — e falta um. Mesmo com `primario = ReSTIR_SHaRC`, o DDGI segue atendendo TRES consumos
    //    de superficie que ninguem mais atende:
    //
    //      - fill de folhagem;
    //      - termo traseiro de subsurface;
    //      - TRANSLUCIDOS do ForwardBlend, que nao recebem a textura do ReSTIR GI.
    //
    //    Consequencia direta para o seletor: `DDGISurfaceAvailable()` NAO pode virar
    //    `EffectivePrimary() == DDGI`. Se virar, escolher SHaRC apaga as tres de uma vez e em
    //    silencio — uma mudanca de imagem que ninguem pediu, escondida dentro de uma mudanca de
    //    politica que prometia so trocar o terminador dos raios.
    //
    //    A regra: `DDGIVolumeLive() && EffectivePrimary() != Off`. O atlas ilumina superficie
    //    enquanto EXISTIR indireto de superficie, seja ele quem for.
    //
    //    ⚠️ E ISSO MUDA COMO O MANIFESTO SE LE: `fallback: black` NAO significa "nenhum DDGI em
    //    superficie" enquanto os auxiliares existirem. Ele descreve o terminador dos RAIOS. Quem
    //    quiser medir superficie sem nenhum DDGI precisa de `primario = Off` — ou de um campo
    //    proprio, se um dia for util distinguir os auxiliares ligados dos desligados.
    // ============================================================================================
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
