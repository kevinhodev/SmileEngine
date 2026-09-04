# Fase 2 — estado da implementação

Data: 2026-09-04

Estado: **em andamento**. A tabela de materiais está entregue; instâncias, meshes e o formato-base
ainda não.

Base: Fase 1 fechada em `439e7b4`. Nada da Fase 1 foi reaberto.

## O que foi entregue

| commit | slice |
|---|---|
| `0dd9ad7` | `FGpuMaterialRecord`: a linha canônica de material, extraída do snapshot por instância |
| `b9055da` | tabela residente na GPU, `FRTInstanceGeo` de 100 B → 20 B, espelho HLSL, fim do dreno |
| `4745a70` | OMM versionado pelas quatro entradas do bake |
| (este) | telemetria de bytes persistentes e bytes sujos por frame |

### A linha canônica

`FGpuMaterialRecord` (64 B, `Engine/Include/Smile/Graphics/Scene/GpuMaterialRecord.h`) é função
pura de `FMaterial`. Cinco índices bindless viraram uma base de descritor mais deslocamentos fixos
— o `FMaterial` já reserva os oito descritores contíguos, então os cinco nunca puderam divergir.
`HasAlbedo` e `TwoSidedRT` viraram os dois bits que sempre foram.

A derivação foi provada idêntica à anterior: o teste carrega uma transcrição literal do
`FillInstanceGeo` de antes da fase como oráculo e compara treze campos em 512 combinações de mapas,
modos e estado de finalização. `FromParts` existe para que o ramo "material finalizado" seja
alcançável sem device — `MappedCBV` e `SRVTableStart` são privados e só o `Finalize` os preenche.

### A tabela

`FGpuMaterialTable` (`Graphics/Scene/GpuSceneTables.h`) é um buffer em DEFAULT heap com uma linha
por material, mais a linha 0 reservada ao material default. As linhas sujas sobem por
`CopyBufferRegion` no topo da command list do frame — antes de qualquer leitor, inclusive o da fila
de compute, porque o trabalho async é submetido com `SubmitAfter` sobre um segmento posterior
daquela mesma lista.

Uma linha é suja por **conteúdo**, não por aviso: o editor sinaliza "algum material mudou" sem
dizer qual, e aplicar um sidecar toca dezenas de uma vez. Recalcular todas as linhas e comparar
custa ~14 KB de memcmp nas cenas atuais e devolve exatamente o conjunto que mudou.

O descritor da tabela viaja dentro do snapshot por instância (`InstanceGeo::MaterialSrv`) em vez de
um cbuffer. Assim os treze shaders que já leem o snapshot alcançam a tabela sem root signature
nova: eles já indexam o heap direto para VB/IB/payload de triângulo. Custa 4 B por instância e
economiza 84.

### O snapshot por instância

`FGpuInstanceGeo`, 20 B, só endereço: três descritores de geometria e o par `{MaterialSrv,
MaterialId}`. Mudou de casa — era um struct local de `RaytracingScene.cpp`; a montagem continua
lá, porque quem preenche tem de ser o dono da correspondência com a TLAS, e essa correspondência
(a ordem, com os buracos preservados) é contrato da Fase 1.

Custo por entrada de capacidade de cena caiu de 1072 B para 992 B.

### ABI

`Tests/GpuSceneTablesWarpTests.cpp` monta os bytes com o C++ de produção e os lê com o espelho HLSL
de produção, compilado pelo mesmo DXC dos shaders do jogo, em WARP. Os `static_assert` do C++ não
diziam nada sobre o lado HLSL: trocar dois campos de lugar no espelho compila dos dois lados.

O teste cobre também as **constantes** compartilhadas (deslocamento de textura, dois bits de flag),
porque renumerar um `SMILE_MAT_FLAG_*` mantém o layout certo e troca o significado de um bit em
todo hit de RT.

## Gate de saída — situação

| item | estado |
|---|---|
| atualização de material toca uma linha, não todos os `FRTInstanceGeo` | **feito**, testado em `DiffMaterialRows` |
| o caminho estável não chama `Flush`/`WaitIdle` por edição de material | **feito**: `NotifyMaterialRTStateChanged` perdeu os dois |
| OMM invalidado quando suas ENTRADAS mudam | **parcial** — ver abaixo |
| layouts e versões com testes de ABI, offsets e bounds | **feito para material e instância**; falta mesh |
| cena grande reporta bytes persistentes por tabela e sujos por frame | **feito** (painel + MCP); falta a medida numa cena grande |
| raster CPU e RT mantêm paridade visual | **não medido** — exige GPU |

## OMM: o que foi fechado e o que não foi

O micro-map é derivado de malha + textura de alpha + fator de alpha + cutoff. O guarda anterior
comparava **dois** desses quatro (`BakedBaseAlpha`, `BakedAlphaCutoff`). Trocar a textura de
BaseColor de um material alpha-tested no editor passava por baixo dele e deixava o micro-map
classificando microtriângulos pelo alpha da textura antiga — sem exceção e sem erro, só recorte
errado em folhagem, que uma comparação estática pode não mostrar.

`FOmmInputKey` passa a registrar a identidade das quatro entradas mais o próprio modo alpha-test
(desligá-lo troca o BLAS exigido). O importador carimba; micro-map sem carimbo não casa com nada,
então um caminho de criação novo que esqueça de carimbar falha alto.

Os dois caminhos que ainda podiam mover uma entrada passaram a **recusar**, como o toggle de Alpha
Test e o Alpha Cutoff já recusavam: a troca do slot BaseColor e o sidecar — que é arquivo editável
à mão, nunca passou pelos setters e é aplicado DEPOIS do bake.

**O que falta, e por quê.** O gate pede "o `FOpacityMicromapData` é substituído/versionado **e os
BLAS afetados são reconstruídos**". Versionar está feito; reconstruir não. Um rebake em runtime
precisa dos vértices da malha e dos pixels da textura, e o runtime não guarda nem um nem outro
depois do load (`FGpuMesh` só tem os buffers de GPU; `FTextureCPUData` é local do import). Fazer
isso é reter dados de CPU para os pares alpha-tested ou reler o `.smesh` do disco — trabalho de
tamanho próprio, e a decisão de financiá-lo é do proprietário.

Enquanto não existir, o invariante que **está** garantido é o que protege a imagem: nenhum
micro-map é consumido cuja chave de entradas divirja das entradas vivas. Recusar a mutação sustenta
esse invariante; é uma extensão da política que já valia para duas das quatro entradas.

## Telemetria

`FCpuFrameStats` ganhou cinco campos: bytes residentes, linhas em uso, linhas sujas neste frame,
regiões de cópia e o acumulado de linhas subidas. Chegam ao painel (`Tabela de materiais`) e ao
snapshot MCP (`cpu.materialTable`).

O bloco MCP é **aditivo** e o `kProtocolVersion` não sobe: a regra da v4 é que a versão sobe quando
o cliente passa a EXIGIR um campo, não quando o editor passa a publicar um. O cliente TypeScript
ainda não agrega esse bloco.

`rowsDirty` é o número do gate: zero em todo frame sem edição de material. O acumulado existe
porque o instantâneo é quase sempre zero — um vazamento de um punhado de linhas por frame não
aparece no instantâneo e aparece na soma.

## Falta na fase

- `FGpuSceneInstance`: colunas SoA (meta, bounds, transform, prev-transform) e o `FRTInstanceGeo`
  derivando delas;
- `FGpuMeshRecord`, com `VirtualGeometryId` reservado e não serializado;
- formato-base novo e deliberado (o número 9 fica queimado pelo experimento descartado; o próximo
  é o 10), com recozimento dos 11 pares e novo `Tools/cook-baseline.json`;
- medir bytes persistentes e sujos numa cena grande de verdade;
- comparação visual CPU/RT no mesmo regime da Fase 1 (preset scientific, `N=128`, 4 cenas × 2
  câmeras). **Exige GPU e aval do proprietário.**

## Risco anotado, ainda não medido

O caminho mais quente do RT (`AlphaTestPass`, que roda por CANDIDATO de travessia, não por hit
comitado) passou a fazer duas leituras dependentes onde fazia uma. A hipótese é que não seja perda:
o snapshot por instância vive num UPLOAD heap — memória do host, lida por PCIe em placa discreta —
e o que sai dele por hit caiu de 100 B para 20 B, enquanto a tabela de materiais vive em DEFAULT
heap e cabe inteira em poucos KB (214 materiais na Emerald = ~14 KB). Medir isso exige GPU.
