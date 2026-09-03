# Baselines versionadas

Relatorios de medicao promovidos de `build/bin/Release/Captures/Benchmarks/`, que o git ignora.
Ficam aqui porque uma baseline que nao sobrevive a um clone nao e uma baseline. Sao gravados
minificados; o SHA-256 e do arquivo como esta neste diretorio.

Gerado por `Tools/SmileMCP/scripts/promote-baseline.mjs` — nao editar a tabela a mao.

| arquivo | build | pontos | runs | capturas | cenas | data |
|---|---|--:|--:|--:|---|---|
| `capture-c452b1c-4scenes.json` | `c452b1c` | 0 | 0 | 16 | bistro emerald synthetic_10k synthetic_100k | 2026-09-03 |
| `perf-capture-c452b1c-1M.json` | `c452b1c` | 4 | 4 | 0 | synthetic_1M | 2026-09-03 |
| `perf-f781288-4scenes.json` | `f781288` | 20 | 16 | 0 | bistro emerald synthetic_10k synthetic_100k synthetic_1M | 2026-09-02 |

## SHA-256

- `capture-c452b1c-4scenes.json`  
  `de852cd964bb058fe1a3aae42486caadc2cc1e341d6fda446fa116fe51339db1`
- `perf-capture-c452b1c-1M.json`  
  `e2b24362695c1dffd26aa0b8bff3bcbec0457f55afe1b801d6167829a2a181eb`
- `perf-f781288-4scenes.json`  
  `f1a7f357c358c9f7352bc2bf7f483fb83468a404ce942da4cdc31e88a913344b`

## Notas

- `capture-c452b1c-4scenes.json` — Metade VISUAL das 4 primeiras cenas: 16 capturas (N=128, preset cientifico, TOD 10h). Sem profiling, por isso sem summary/runs.
- `perf-capture-c452b1c-1M.json` — synthetic_1M completa: 4 pontos, 4 runs com GPU por passe, mais PNG+manifesto por slot. Recaptura do que faltava em perf-f781288-4scenes.
- `perf-f781288-4scenes.json` — Perf das 4 primeiras cenas (16 pontos, 16 runs completos). As 4 linhas de synthetic_1M no summary sao so medianas de CPU: o teardown lancou antes de os runs detalhados delas serem publicados. A 1M completa vive em perf-capture-c452b1c-1M.json; Engine/Editor/Shaders sao identicos entre f781288 e c452b1c (git diff vazio), entao as duas comparam.
