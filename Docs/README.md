# Documentação da SmileEngine

Este diretório reúne referências técnicas, protocolos de validação, planos de
trabalho e auditorias de subsistemas. Para instalação e primeiros passos,
consulte o [README principal](../README.md).

> [!IMPORTANT]
> O código é a fonte de verdade. Documentos marcados como **plano** ou
> **auditoria** registram decisões, medições e trabalho pendente; eles não devem
> ser interpretados como uma descrição integral do estado atual da engine.

## Por onde começar

| Objetivo | Documento |
|---|---|
| Compilar e executar o projeto | [README principal](../README.md) |
| Entender a arquitetura da engine | [Arquitetura](ARCHITECTURE.md) |
| Produzir capturas comparáveis | [Protocolo de captura](CAPTURE-PROTOCOL.md) |
| Automatizar build, execução e profiling | [SmileMCP](../Tools/SmileMCP/README.md) |
| Trabalhar em GI e radiance cache | [Plano SHaRC](SHARC-PRIMARY-GI-PLAN.md) |
| Trabalhar em DDGI e cascatas | [Auditoria DDGI](DDGI-AUDIT.md) |
| Trabalhar em mesh lights e ReSTIR DI | [Plano de mesh lights](MESH-LIGHTS-PLAN.md) |

## Tipos e status

- **Referência ativa:** descreve o código atual e deve acompanhar mudanças de
  arquitetura.
- **Protocolo estável:** procedimento repetível, com critérios de validação
  definidos.
- **Plano ativo:** sequência de implementação e gates; o bloco de estado no
  início informa o ponto de retomada.
- **Auditoria:** diagnóstico datado, decisões aplicadas e dívida conhecida.

## Referência e protocolos

| Documento | Tipo | Estado |
|---|---|---|
| [Arquitetura da engine](ARCHITECTURE.md) | Referência ativa | Revisão técnica parcial em 2026-08-19 |
| [Protocolo de captura determinística](CAPTURE-PROTOCOL.md) | Protocolo estável | Calibrado com 128 frames de aquecimento |
| [Motion vectors temporais](TEMPORAL_MOTION_VECTORS.md) | Nota técnica | Implementação atual |

## Planos de trabalho

| Documento | Estado resumido |
|---|---|
| [SHaRC como GI primário](SHARC-PRIMARY-GI-PLAN.md) | Fases 0–5 concluídas; Fase 6 com gates de runtime pendentes; Fase 7 em andamento |
| [Mesh lights em escala](MESH-LIGHTS-PLAN.md) | Fases 0 e 0.5 concluídas; fases seguintes permanecem como plano |

Os números de performance desses documentos pertencem às cenas, configurações e
hardware identificados em cada medição. Eles não representam benchmarks gerais
da engine.

## Auditorias e contratos

| Documento | Escopo | Estado resumido |
|---|---|---|
| [DDGI](DDGI-AUDIT.md) | Invalidação, atlas e cascatas | Núcleo validado; scrolling implementado e ainda sem validação de runtime registrada |
| [CSM](CSM-AUDIT.md) | Sombras direcionais | Fases 1–4 aplicadas; fila estrutural aberta |
| [Knobs de render](KNOBS-AUDIT.md) | Estado, invalidação e editor | Auditoria histórica; migração principal concluída |
| [Oceano FFT](OCEAN-AUDIT.md) | Espectro, clipmap e integração | Contrato validado; limitações conhecidas documentadas |

## Recursos visuais e atribuições

- [Pasta reservada para screenshots](Screenshots/README.md)
- [Diagrama da arquitetura](SmileRTArchitecture.svg)
- [Referência visual do editor de materiais](material-editor-ref.svg)
- [Referência visual das configurações](settings-window-ref.svg)
- [Créditos das texturas de terreno](../Assets/Textures/Terrain/FONTE.md)

## Convenções editoriais

- A prosa é escrita em português do Brasil.
- Identificadores, nomes de arquivos e nomes exibidos pelo profiler permanecem
  como aparecem no código.
- Datas indicam a última revisão registrada, não uma garantia de sincronização.
- Em caso de divergência, prevalecem o código, os testes e as medições
  reproduzíveis.
