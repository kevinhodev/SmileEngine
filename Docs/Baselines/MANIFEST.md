# Baselines versionadas

Relatorios de medicao promovidos de `build/bin/Release/Captures/Benchmarks/`, que o git ignora.
Ficam aqui porque uma baseline que nao sobrevive a um clone nao e uma baseline. Sao gravados
minificados; o SHA-256 e do arquivo como esta neste diretorio.

Gerado por `Tools/SmileMCP/scripts/promote-baseline.mjs` — nao editar a tabela a mao.

| arquivo | build | pontos | runs | capturas | cenas | proveniencia | data |
|---|---|--:|--:|--:|---|---|---|
| `capture-c452b1c-4scenes.json` | `c452b1c` | 0 | 0 | 16 | bistro emerald synthetic_10k synthetic_100k | so carimbo | 2026-09-03 |
| `perf-capture-c452b1c-1M.json` | `c452b1c` | 4 | 4 | 0 | synthetic_1M | so carimbo | 2026-09-03 |
| `perf-f781288-4scenes.json` | `f781288` | 20 | 16 | 0 | bistro emerald synthetic_10k synthetic_100k synthetic_1M | so carimbo | 2026-09-02 |

## SHA-256

| arquivo | sha256 |
|---|---|
| `capture-c452b1c-4scenes.json` | `dfe88f459b26927e238c575bb7e8cc8423037029d6bfcfa1ca56564530cbb5fa` |
| `perf-capture-c452b1c-1M.json` | `be10d0b9dd865ad955b32e5ba74d3ba1522859ae0138d90e8c587372b6e25e3b` |
| `perf-f781288-4scenes.json` | `f1a7f357c358c9f7352bc2bf7f483fb83468a404ce942da4cdc31e88a913344b` |

## Notas

- `capture-c452b1c-4scenes.json` — Metade VISUAL das 4 primeiras cenas: 16 capturas (N=128, preset cientifico, TOD 10h). Sem profiling, por isso sem summary/runs. Proveniencia legada: anterior ao registro de gitState/fingerprints.
- `perf-capture-c452b1c-1M.json` — synthetic_1M completa: 4 pontos, 4 runs com GPU por passe, mais PNG+manifesto por slot. Rodada com --capture, entao os tempos estao ~1,4% acima do equivalente sem captura. Proveniencia legada: anterior ao registro de gitState/fingerprints.
- `perf-f781288-4scenes.json` — Perf das 4 primeiras cenas (16 pontos, 16 runs completos). passed:false e incompleto pelo MESMO motivo: uma assercao de teardown disparou depois da ultima medicao, antes de os runs detalhados da 1M serem publicados; as 4 linhas de synthetic_1M no summary sao so medianas de CPU, e a 1M completa vive em perf-capture-c452b1c-1M.json. Proveniencia legada: a rodada e anterior ao registro de gitState/fingerprints, e a limpeza da arvore so consta do carimbo da build (f781288). Engine/Editor/Shaders identicos entre f781288 e c452b1c.
