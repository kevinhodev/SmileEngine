# GPU-driven — Fase 5: fechamento

Status: **fechada em 2026-09-15**. O caminho permanece desligado por default para preservar o A/B;
ligá-lo exige material e transform bindless efetivamente ativos.

## O que foi entregue

- `FGpuIndirectDrawArgs` e a command signature foram provados com `ExecuteIndirect` real em WARP.
- A tabela de meshes publica offsets e identidade do par VB/IB; a de materiais publica a classe de
  pipeline; a de instâncias fornece slot, mesh e material estáveis.
- A CPU mantém regiões estáveis por `{PipelineClass, GeometryPoolId}`. A GPU executa
  reset → expand → classify/count → scatter → finalize, sem readback no caminho de decisão.
- A finalização é fail-closed: overflow, versão divergente, cursor, região, capacidade ou qualquer
  canário zeram o `DrawCounts` inteiro. Somas são limitadas antes da adição, para `uint` modular não
  transformar corrupção em zero plausível.
- Z-prepass opaco e G-buffer consomem o stream. Durante o culling a CPU monta uma única lista com o
  complemento; os laços de submissão e o sort visitam só as exceções quando o indireto está ativo.
- Material nulo ou não finalizado resolve para a mesma linha do material ativo nos metadados da
  instância e no raster. `SetMaterial` invalida a cena e o indireto exige uma linha canônica para esse
  material. A partição CPU/GPU não consulta duas associações diferentes.
- Toggle, disponibilidade, chamadas por passe, comandos, status/canários atrasados e footprint sobem
  pelo protocolo MCP 14. O manifesto de captura registra o braço efetivo.

Commits finais da integração:

- `087b45f` — rejeita overflow modular na finalização;
- `df1c02a` — integra a cadeia e o fallback por exceção no renderer;
- `d3758f6` — protocolo 14 e aparelho OFF/ON/OFF em hardware.
- `6645dd9` — material ativo e metadados da instância usam a mesma identidade, com prova integrada.

## Gate automatizado

- Release: `ctest` **70/70**.
- WARP: layout, tabelas, expansão, classify/scatter/finalização, command processor e cadeia integrada.
- TypeScript e borda do editor: protocolo 14 exige cada campo novo; ausência, tipo, sinal e precisão
  incorretos são recusados.
- O aparelho de hardware é `npm run validate:gpu-driven:phase5`; o comparador é
  `compare-gpu-driven-phase5.py <relatório.json>`.

## A/B em hardware

Relatório bruto desta rodada:

`build/bin/Release/Captures/Validation/phase5/phase5-validation-2026-09-16T15-11-59-161Z.json`

Relatório com comparação de pixels:

`build/bin/Release/Captures/Validation/phase5/phase5-validation-2026-09-16T15-11-59-161Z-compared.json`

Os dois cenários rodaram OFF→ON→OFF no mesmo processo, sem reload.

### Emerald

- draws CPU do G-buffer: **1.589 → 867**;
- comandos GPU: **1.463**, confirmados pelo readback;
- `SubmitMs`: **1,614 → 1,232**;
- `BatchSortMs`: **0,054 → 0,016**;
- `gpuStatus=1`, `gpuReason=0`;
- ON versus média dos OFF: `meanAbs=0,160`; o piso OFF→OFF foi `0,174`.

### Sintética de 1M

- draws CPU do G-buffer: **763.408 → 177.578**;
- comandos GPU: **750.000**, confirmados pelo readback;
- `SubmitMs`: **243,0 → 40,2 ms**;
- `BatchSortMs`: **54,8 → 4,98 ms**;
- `gpuStatus=1`, `gpuReason=0`;
- ON versus média dos OFF: `meanAbs=1,032`; o piso OFF→OFF foi `3,601`.

As capturas científicas ON ficaram abaixo do próprio ruído OFF→OFF também na fração de pixels com
erro máximo por canal acima de 5. O log não contém erro D3D12, remoção de device ou validação GPU.

## Limites deliberados

- Esta fase só emite `OpaqueOneSided`. Masked e two-sided ficam no fallback até a Fase 7; blend,
  terreno, água e helpers permanecem CPU-driven.
- Não há culling GPU ainda. A cadeia pode desenhar todos os candidatos suportados; frustum e HiZ
  continuam produzindo a lista visível necessária ao fallback e ao editor. Essa migração é a Fase 6.
- O readback dos contadores é apenas telemetria atrasada. Nenhuma decisão de draw depende dele.
- O helper de inspeção da janela do Codex não iniciou nesta rodada por uma falha local de ACL do
  sandbox. Isso não afetou a validação: o editor real produziu seis capturas, perfis, manifestos e
  logs, e a comparação foi feita diretamente sobre os PNGs salvos.

## Próximo passo

A Fase 6 move frustum/oclusão para a GPU, mantém o HZB residente e deixa o readback apenas como
métrica. Ela começa sobre este stream já validado; não reabre a ABI da Fase 5.
