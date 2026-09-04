# Fase 1 — relatório de validação

Data: 2026-09-04

Estado: fechada; Fase 2 não iniciada

Commits finais da continuação: `35ae074` (identidade) e `9c696be` (recuperação do viewport)

## Resultado

A identidade durável deixou de ser a posição na `RenderableList`. A ordem autoral continua no
vetor, `ObjectId` continua sendo a identidade do editor e `FGpuSceneHandle {slot, generation}`
passou a identificar a residência GPU. O raster e o método de desenho continuam CPU-driven.

Os quatro consumidores foram migrados:

| consumidor | contrato depois da migração |
|---|---|
| Picking | o ID do passe indexa uma tabela capturada `{GpuHandle, ObjectId}`; a resolução exige o mesmo objeto e a mesma residência |
| TLAS | o `InstanceID` é endereço local daquele frame; tabela anterior, remap atual e tabela atual resolvem `{slot, generation}` antes de um leitor temporal usar o índice atual |
| HiZ | o dispatch compacto guarda um handle por entrada; o readback atrasado só publica para handle ainda vivo e nunca alcança o ocupante seguinte do slot |
| TemporalMotion | `PrevMVP` raster e transforms de RT são guardados por slot + generation; ocultação esquece apenas o objeto afetado |

Os leitores temporais reais compartilham o mesmo resolvedor: `DualMotion`, `ReflectionMotion` e
`ReSTIRDISpatial`. Buracos da TLAS são preservados deliberadamente para manter alinhamento com
`InstanceGeo`; eles carregam entrada inválida na tabela e não viram outra identidade.

## Ciclo de vida

- `Free`, `Live`, `Retired` e `Exhausted` são estados distintos;
- `IsAlive` exige estado `Live` e generation igual;
- remoção aposenta imediatamente com uma fence por fila;
- `Collect` percorre somente a lista de aposentados e libera depois das duas fences;
- reload preserva o allocator, reutiliza a capacidade e incrementa generation;
- wrap de generation leva o slot a `Exhausted` depois das fences;
- capacidade, vivos, aposentados e esgotados chegam ao painel e ao MCP.

## Evidência automatizada

Build Release completo e `ctest` em 2026-09-04: **57/57**.

- `Smile.GpuSceneHandle`: estados, stale handle, reuso, duas filas, wrap e custo proporcional aos aposentados;
- `Smile.SlotKeyedHistory`: histórico por handle, `Forget`, slot reciclado e handle zerado;
- `Smile.TemporalIdentityTable`: tabela/remap, contagens, buracos, HiZ atrasado e gates dos três leitores;
- `Smile.TemporalResolveWarp`: o shader de produção compilado pelo DXC roda em WARP com 11 casos;
- `Smile.RendererIdentity`: RTX/DXR real com cena sintética, picking pendente, remoção, reuso,
  hide/unhide, primeiro/meio/último e reload sem crescimento do pool.

Também foram executadas regressões intencionais e restauradas:

- recolocar `CancelPending()` fez o teste real falhar em `erase cancelled the pending pick`;
- aceitar receptor de outro objeto fez a sonda WARP falhar;
- aceitar resultado HiZ de handle `Retired` fez o teste de identidade falhar.

## Comparação visual

Regime igual ao pacote da Fase 0: preset scientific, `N=128`, mesmas câmeras, resolução e opções.
Todos os metadados comparados coincidiram. Referência:
`Docs/Baselines/Reference/capture-c452b1c-4scenes/`.

| cena/slot | MAE por canal | PSNR (dB) | SSIM | pixels com delta > 20 |
|---|---:|---:|---:|---:|
| Bistro 0 | 0,114 | 56,07 | 0,999752 | 0,0000% |
| Bistro 1 | 0,116 | 55,91 | 0,999704 | 0,0006% |
| Bistro 2 | 0,025 | 62,79 | 0,999878 | 0,0000% |
| Bistro 3 | 0,076 | 58,49 | 0,999835 | 0,0000% |
| Emerald 0 | 0,279 | 46,62 | 0,999392 | 0,0548% |
| Emerald 1 | 0,720 | 37,84 | 0,990831 | 1,1661% |
| Emerald 2 | 0,010 | 67,93 | 0,999945 | 0,0000% |
| Emerald 3 | 0,531 | 43,72 | 0,997748 | 0,0939% |

O maior MAE, 0,720, permanece abaixo do piso de ruído de 1,08 já medido entre duas capturas da
própria build de referência. Inspeção dos pares Emerald 1 e 3 não mostrou troca de objeto,
ghosting, oclusão herdada nem mudança estrutural da imagem.

As capturas desta validação ficaram em `build/bin/Release/Captures/` e não substituem a referência
versionada da Fase 0.

## Tela preta na abertura

Durante a validação, a Emerald reproduziu o viewport preto com menus e painéis responsivos.
Minimizar/restaurar não recuperou. O processo estava ocioso e o snapshot reportava `frameIndex=0`,
portanto nenhum frame havia sido concluído: não era shader escuro nem perda de apresentação.

A causa era o `RedrawTimer` single-shot. Caminhos transitórios de `OnRenderTimer` retornavam quando
um frame/job estava em voo sem rearmar o timer. `showEvent` e `BecameIdle` podiam observar lados
opostos da mesma transição e ambos perder o único wake-up.

O viewport agora agenda uma retentativa coalescida de 50 ms nesses caminhos e para de tentar assim
que um frame é aceito. O status MCP só publica `ready=true` depois de pelo menos um frame concluído,
impedindo uma captura de entrar e ficar presa no frame zero.

Validação: cinco aberturas consecutivas alternando Emerald/Bistro/Emerald/Bistro/Emerald; todas
avançaram (snapshots entre os frames 110 e 691). Em seguida, a captura automatizada completou 8/8
slots e encerrou normalmente.

## Limitações preservadas

- a remoção no editor continua O(N), por decisão explícita desta fase;
- `InstanceID`, transforms e `InstanceGeo` ainda usam índice local do frame; o índice não é
  persistido nem reinterpretado sem as tabelas de identidade;
- os drenos de fila que protegem `InstanceGeo` single-buffer e `MeshLights` permanecem;
- o teste RTX é opt-in (`SMILE_ENABLE_RENDERER_IDENTITY_TESTS`) em máquinas sem DXR;
- a comparação visual ainda é uma etapa de validação, não um gate automático de tolerância.
