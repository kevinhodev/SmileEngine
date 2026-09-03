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
| `capture-c452b1c-4scenes.json` | `2277a487a63c1caa62db44e8eb68e2b8e14bf2e1acf905f26de6e34ee6c98ded` |
| `perf-capture-c452b1c-1M.json` | `9fa8443b567eb4a70338c9474b87d4d77887d27710088c69daf4978df9ac80c9` |
| `perf-f781288-4scenes.json` | `f1a7f357c358c9f7352bc2bf7f483fb83468a404ce942da4cdc31e88a913344b` |

## Notas

- `capture-c452b1c-4scenes.json` — Metade VISUAL das 4 primeiras cenas: 16 capturas (N=128, preset cientifico, TOD 10h). Sem profiling, por isso sem summary/runs.
- `perf-capture-c452b1c-1M.json` — synthetic_1M completa: 4 pontos, 4 runs com GPU por passe, mais PNG+manifesto por slot. Recaptura do que faltava em perf-f781288-4scenes. Rodada com --capture, entao os tempos estao ~1,4% acima do equivalente sem captura.
- `perf-f781288-4scenes.json` — Perf das 4 primeiras cenas (16 pontos, 16 runs completos). passed:false e incompleto pelo MESMO motivo: uma assercao de teardown disparou depois da ultima medicao (o editor da 1M nao morreu em 180 s apos close_editor), antes de os runs detalhados da 1M serem publicados. As 4 linhas de synthetic_1M no summary sao so medianas de CPU; a 1M completa vive em perf-capture-c452b1c-1M.json. Engine/Editor/Shaders sao identicos entre f781288 e c452b1c (git diff vazio), entao as duas comparam.
