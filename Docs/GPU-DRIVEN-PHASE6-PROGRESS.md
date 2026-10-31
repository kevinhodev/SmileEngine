# GPU-driven — Fase 6

Estado em 2026-10-31: implementação concluída, validação final em hardware em andamento.
`gpuCulling` continua desligado por default, e exige main view indireto/material/transform bindless.

## Contrato implementado

- Depth: expand -> frustum GPU -> classify/count -> scatter -> finalize -> ExecuteIndirect.
- G-buffer: HZB atual do depth -> reutilizar expansão e rejeitados de frustum -> oclusão ->
  classify/count -> scatter -> finalize -> ExecuteIndirect.
- A matriz de projeção é a mesma jitterada que escreveu o depth. Frustum recebe um pixel de
  folga; o retângulo de oclusão é arredondado para fora e inflado. Cruzamento do near plane,
  câmera dentro do bound e bounds inválidos/degenerados não provocam rejeição por oclusão.
- Dois bancos de constantes, descritores e readbacks por frame slot. Recursos DEFAULT únicos,
  com transições/UAV barriers na fila direta. A cadeia tardia executa depois dos draws de depth.
- Rejeições de visibilidade são desfechos normais na contabilidade compartilhada; canários
  continuam invalidando todo o count buffer. Recusa antes de gravar late preserva o stream de depth.
- Sem `HiZ.IsVisible`, `HiZ.BeginFrame` ou `HiZ.RecordTest` no braço GPU. Nenhuma exceção para o
  selecionado nesse braço, nenhuma leitura de contador participa da decisão de draw.
- Masked, two-sided, blend e demais exceções seguem na CPU, com frustum e sem oclusão atrasada.
  As sombras ainda usam AllItems. A migração desses consumidores é a Fase 7.
- Culling/sort CPU visita só as exceções; opacos suportados voltam à lista CPU para picking sob
  demanda. `VisibleItems` mede a lista CPU; comandos/rejeições GPU têm métricas próprias.
- Protocolo MCP 15: `gpuCulling`, `frustumCulling`, `occlusionCulling`, rejeições separadas;
  captura registra o toggle efetivo.

## Provas automatizadas

O `Smile.GpuIndirectMainView` executa os shaders de produção em WARP e lê argumentos, contagens,
slots e status. Expectativas geométricas são literais: frente/atrás de um HZB conhecido, fora do
frustum, near crossing, câmera dentro do bound, teleporte, perspectiva reverse-Z, jitter e
resolução ímpar. Um snapshot intermediário prova os counts do depth antes da cadeia tardia.

O `Smile.RendererIdentity` liga o caminho no renderer real, confirma status da GPU, faz picking
do objeto rotacionado/escalado, redimensiona para 641x361, muda render scale para 0,75, restaura
e repete o picking. Os testes da borda MCP incluem os dois contadores novos.

Quatro negativos reintroduzidos individualmente e restaurados:

- sem frustum no shader real: rejeitados viram comandos e o multiconjunto diverge;
- sem oclusão no shader real: o objeto atrás do HZB volta ao count/args;
- sem a proteção de near/câmera: a caixa que envolve a câmera é rejeitada indevidamente;
- bancos de constantes compartilhados: o snapshot do depth tem 3 comandos em vez de 4, ainda
  que a contagem final do G-buffer esteja correta.

## Aparelho de hardware

`npm run validate:gpu-driven:phase6`: Emerald slot 0, 1M slot 0 (tudo no frustum) e 1M slot 1
(câmera interna). Cada cenário alterna OFF -> ON -> OFF sem reload entre os braços, grava perfis,
capturas com manifesto e confere logs/canários. O comparador de pixels da Fase 5 aceita o mesmo
schema de relatório. Não rodar testes/builds concorrentes durante as medidas de desempenho.

Rodadas de desenvolvimento encontraram e corrigiram duas lacunas do aparelho: o slot 0 da
sintética tem frustum rejection zero por construção, e a consulta de logs aceita até 1000 linhas.
O custo inicial também revelou culling/sort CPU duplicado dos opacos; foi retirado antes da
medição final. Esses relatórios de desenvolvimento não são a baseline de fechamento.

Resultados finais e gate de desempenho serão registrados após a rodada sobre o commit implementador.
