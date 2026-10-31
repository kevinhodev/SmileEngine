# GPU-driven — Fase 6

Estado em 2026-10-31: **implementação e gate técnico concluídos**. A baseline formal não foi
promovida: os manifestos têm sufixo `-dirty`, explicado abaixo.
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

## Resultado final

Commits de renderização: `81c3a83` (cadeia e culling) e `ac4c07d` (partição pelos IDs publicados,
sort único e comparador de custo). O segundo passou novamente nos seis testes afetados, incluindo
o renderer real, e compilou em Release e Debug. A suíte Release completa passou **72/72**; os
dois testes WARP da cadeia também passaram em Debug.

Rodada final sem testes/builds concorrentes: RTX 3060 Ti, Release, **1300x730 nativo**, preset
`controlled_native`, TOD 10h, material/transform/indirect ligados, frustum e oclusão ligados.
OFF e ON variam somente `gpuCulling`, no mesmo processo e bookmark, sem reload entre braços.
24 amostras por braço na Emerald e 1M externa; 48 por braço na 1M interna. Cada amostra exige
cadeia ativa, contadores válidos, status 1 e motivo zero.

Relatórios:

- `build/bin/Release/Captures/Validation/phase6/phase6-validation-2026-10-31T21-06-28-245Z.json`
- `build/bin/Release/Captures/Validation/phase6/phase6-validation-2026-10-31T21-06-28-245Z-compared.json`

Medianas de `Frame (GPU)` em ms, na ordem OFF-A -> ON -> OFF-B:

- Emerald slot 0: **18,692 -> 18,590 -> 18,752**. Comandos: 1.463 -> 748.
  p95: 19,053 -> 19,019 -> 19,810. Rejeições ON: 794 por frustum, ~62 por oclusão.
- 1M slot 0, todo o volume no frustum: **339,537 -> 294,735 -> 358,929**.
  Comandos: 750.000 -> ~706.226. p95: 347,695 -> 302,178 -> 413,074.
  Frustum zero é esperado neste bookmark; há ~58.450 rejeições de oclusão entre todos os candidatos.
- 1M slot 1, câmera interna: **345,794 -> 235,597 -> 350,383**.
  Comandos: 750.000 -> ~68.329. p95: 362,736 -> 256,227 -> 367,159.
  Rejeições: ~901.636 por frustum e ~7.332 por oclusão.

Os comandos contam apenas opacos suportados; rejeições consideram todos os candidatos da cadeia.
Os agregados podem ser fracionários porque são medianas de contagens inteiras de frames distintos.
O custo CPU `cullSort` na 1M externa caiu de 70,85/73,49 para 28,56 ms. O `submit` CPU aumentou
de ~46-49 para ~65 ms: as exceções não usam mais oclusão atrasada e desenham conservadoramente.
O caminho crítico total ainda caiu; masked/two-sided continuam sendo trabalho da Fase 7.

Pixels ON contra a média dos OFF ficaram abaixo do ruído OFF/OFF nos três cenários:
`meanAbs` 0,128 contra 0,208 (Emerald), 1,397 contra 3,829 (1M externa) e 0,734 contra 1,407
(1M interna). A fração de pixels com erro >5 também passou. Logs sem erro D3D12/DXGI/device removal.

`compare-gpu-driven-phase6.py` exige mediana ON <= maior p95 OFF e p95 ON <= maior p95 OFF +
deriva medida entre as medianas OFF. O relatório guarda as distribuições completas e esse teto.
O teste negativo do próprio comparador foi a rodada anterior: a mediana interna melhorava, mas
o p95 ainda regredia; o gate a rejeita. A rodada final passou nos três cenários.

## Proveniência e limite de promoção

O binário registra **`ac4c07d-dirty`**. O diff rastreado estava vazio na medição; os itens não
rastreados eram `Docs/OMM-AUDIT.md` (apareceu durante a sessão e foi preservado) e o cache Python
do comparador. O mecanismo de build marca qualquer arquivo não rastreado como dirty, de propósito.
Não alterei esse mecanismo nem escondi o rascunho. O cache gerado foi removido ao concluir.

Essas capturas são evidência de shakedown do código commitado, **não uma baseline promovida**.
Uma promoção formal requer repetição com árvore limpa, preservando o rascunho do proprietário.
Os rótulos de visibilidade do editor foram ajustados para dizer explicitamente "Lista CPU";
essa alteração de apresentação não modifica a cadeia de renderização medida.

## Próximo passo

Fase 7: pass masks, streams de sombras e migração de masked/two-sided, preservando blend na CPU.
Não iniciada por este fechamento técnico.
