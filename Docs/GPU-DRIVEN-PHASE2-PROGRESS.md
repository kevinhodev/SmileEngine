# Fase 2 — estado da implementação

Data: 2026-09-04

Estado: **em andamento, não promovível.** As tabelas de materiais e de meshes estão entregues e
validadas em runtime; os quatro findings da revisão estão corrigidos. Faltam **duas decisões do
proprietário**, e são elas que seguram o encerramento da fase:

1. **`FGpuSceneInstance`** — desenhada e orçada, não implementada. Ver a seção do layout.
2. **OMM** — o gate pede substituir/versionar o micro-map **e reconstruir os BLAS afetados**.
   Versionar está feito; reconstruir não. Ou se implementa o rebake em runtime (o que exige reter
   dados de CPU que o runtime hoje descarta), ou o gate muda formalmente para aceitar a política de
   recusa. Não dá para declarar fechado do jeito que está.

Base: Fase 1 fechada em `439e7b4`. Nada da Fase 1 foi reaberto.

## O que foi entregue

| commit | slice |
|---|---|
| `0dd9ad7` | `FGpuMaterialRecord`: a linha canônica de material, extraída do snapshot por instância |
| `b9055da` | tabela residente na GPU, `FRTInstanceGeo` de 100 B → 20 B, espelho HLSL, fim do dreno |
| `4745a70` | OMM versionado pelas quatro entradas do bake |
| `9186d93` | telemetria de bytes persistentes e bytes sujos por frame |
| `b65063c` | `FGpuMeshRecord` + `FGpuMeshTable`, dona dos descritores de geometria |
| `ab5f466` | layout e orçamento da tabela de instâncias |
| (este) | os quatro findings da revisão de runtime |

## Findings da revisão (2026-09-04)

Validação em runtime na RTX 3060 Ti — sintética de 1M, comparação visual oficial 8/8, Bistro sem
cliff — achou quatro defeitos. Os quatro estão corrigidos; dois deles **não são regressões da
Fase 2**, e vale registrar a diferença porque ela muda o conserto.

### P1 — descritor de textura reescrito com trabalho em voo

`FTextureSRVHeap::CreateSRV` escreve no heap **shader-visible**, não só no staging. Trocar a
textura de um material reescrevia o slot no lugar, sem fence: uma command list já submetida que
referencie aquele slot ainda vai lê-lo, e isso é modificar descritor em uso.

**Não foi a Fase 2 que abriu isso.** O dreno anterior morava no `NotifyMaterialRTStateChanged`,
consumido no *topo do frame seguinte* — depois da escrita. Ele protegia o snapshot `InstanceGeo`,
que é outro recurso; este slot nunca esteve coberto. Por isso o conserto não é "preservar o dreno
para trocas de textura": um dreno no mesmo lugar de antes continuaria chegando tarde. O dreno agora
está **antes** da escrita, dentro de `Renderer::UpdateMaterialTextureSlot`, que é o funil.

O conserto estrutural — versionar/aposentar slot de descritor pelas duas fences, como o
`FGpuSceneSlots` já faz para residência de cena — fica para a Fase 3, que é quando o raster vira
bindless e passa a rotacionar descritor de material de verdade.

### P1 — subsurface não sujava a linha

`SetSubsurfaceColor`/`SetSubsurfaceIntensity` usavam o canal só-raster, e `SubsurfaceColor` é campo
da linha canônica, lido pelo path tracer como `TransColor`. Também é anterior à Fase 2 (o
`FillInstanceGeo` já lia o campo e o canal já era o errado); o que mudou é que antes qualquer outra
edição do mesmo material corrigia por acidente, ao reescrever a cena inteira.

Corrigido nos dois setters. E, porque a auditoria dos onze setters mostrou que o erro é de *canal*
e não de campo, `TouchSelected` passou a marcar a linha sempre: hoje isso custa zero quando o campo
não está na linha (a tabela suja por conteúdo — uma edição de normal strength resulta em zero linha
suja e zero cópia), e antes da Fase 2 teria custado um dreno por tick. O `TouchSelectedRT` continua
seletivo para o que de fato invalida histórico e flags de TLAS.

### P2 — a telemetria não chegava a quem mede

Os blocos existiam na amostra crua e não no `summarizeProfile`, que é o que a régua lê com o
`includeSamples` default. O cliente TypeScript agora tipa, **exige** e resume os dois blocos, e o
protocolo subiu para **5** — pela mesma regra da v4: a versão sobe quando o cliente passa a exigir
o campo, não quando o editor passa a publicá-lo. A validação de borda ganhou casos negativos para
cada campo dos dois blocos.

### P2 — `residentBytes` valia um terço

Contava só o buffer DEFAULT. Cada tabela mantém também dois slices de staging **persistentes** —
criados no `Create` e mapeados o tempo todo, não alocados por upload. Agora são duas grandezas
separadas (`residentBytes` = VRAM, `stagingBytes` = host), porque não saem do mesmo orçamento, e o
painel mostra a soma. A conta virou função pura `FGpuTableStorage::FootprintFor`, testável sem
device — sem isso o campo não tinha guarda nenhuma, que foi como ele nasceu errado.

### Cobertura dos consertos

Dois têm teste; dois não, e é honesto dizer qual é qual:

| conserto | prova |
|---|---|
| footprint | `TestarFootprint`; zerar o staging na conta deixa 3 asserções vermelhas |
| MCP | `Smile.EditorBridgeValidation`; tirar `meshTable` da validação deixa 5 casos vermelhos |
| dreno do descritor | **sem teste automatizado** — é hazard de lifetime de GPU. Quem prova é a debug layer do D3D12 numa sessão Debug do editor trocando textura de material |
| subsurface | **sem teste automatizado** — é fiação de bridge Qt. Observável pela régua: editar subsurface tem de levar `cpu.materialTable.rowsDirty` de 0 para 1 num frame, e voltar a 0 |

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

### A tabela de meshes

Vale ser honesto sobre o que ela **não** dá: zero byte de economia no snapshot por instância —
`VertexSrv`/`IndexSrv` saem, `MeshSrv`/`MeshId` entram, e ele continua em 20 B. O ganho é
estrutural, e são três coisas concretas:

1. **Uma casa para o que é da malha.** Bounds locais, contagens e os descritores bindless de
   geometria. Os bounds ainda vivem duplicados em cada `FRenderable` que usa a malha (copiados do
   `SMeshEntry` no load); a duplicação sai quando a tabela de instâncias existir, na Fase 4.
2. **A posse dos descritores de geometria saiu do `FRaytracingScene`** — decisão D12: o ray tracing
   é consumidor da cena, não dono dela. Junto veio a enumeração das malhas únicas, que agora tem um
   dono só; o `Build` lê a lista de lá para dimensionar os BLAS. Isso não é arrumação: aquela
   enumeração já divergiu uma vez, quando o `FDDGI` a reconstruía "com um critério idêntico,
   palavra por palavra".
3. **Um consumidor real, no caminho frio.** O `MeshLightExtract` passou a ler VB/IB da linha da
   malha. É uma extração por cena, então prova a cadeia instância → malha → geometria no renderer
   de verdade sem tocar no caminho quente.

O `TriangleSrv` fica **desnormalizado** no snapshot, e é a única exceção: o `AlphaTestPass` roda
por *candidato de travessia*, não por hit comitado, então em folhagem é chamado várias vezes por
raio. Passar por instância → malha → payload acrescentaria uma leitura dependente exatamente ali.
A decisão D3 autoriza: tabela canônica, snapshot especializado.

`VirtualGeometryId` está declarado e reservado na linha, com valor zero e sem contraparte no
arquivo cozido — ver a decisão de formato no plano.

### O snapshot por instância

`FGpuInstanceGeo`, 20 B, só endereço: o payload por triângulo, o par `{MeshSrv, MeshId}` e o par `{MaterialSrv,
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
| layouts e versões com testes de ABI, offsets e bounds | **feito** para material, mesh e instância; falta a de instâncias da Fase 4 |
| cena grande reporta bytes persistentes por tabela e sujos por frame | **feito e medido** na sintética de 1M: `rowsDirty=0` e `copyRegions=0` em regime estável; `residentBytes` corrigido para incluir o staging |
| formato-base novo e deliberado | **decidido não bumpar** — ver a nota de estado da Fase 2 no plano |
| raster CPU e RT mantêm paridade visual | **feito**: `scientific`, N=128, Bistro+Emerald, 8/8 capturas; MAE máximo 0,900, abaixo do piso de ruído de 1,08 já documentado na Fase 1 |

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

## Tabela de instâncias: layout escolhido e orçamento

Primeiro item do trabalho da fase — "escolher layouts paginados/SoA por consumidor e orçar bytes
por instância residente". O layout está escolhido e orçado aqui; a implementação depende de uma
decisão registrada logo abaixo.

**O que existe hoje.** A mesma instância é subida em **quatro formas diferentes**, e nenhuma delas
é residente — todas são reescritas por frame ou por evento:

| produtor | struct | bytes/instância | quando |
|---|---|---:|---|
| raster | `ObjectConstants` (4 × `Mat44`) | 256 (×2 frames) | por renderável VISÍVEL, todo frame |
| temporal | `FTemporalInstanceTransformGPU` | 128 (×2 frames) | por instância da TLAS, todo frame |
| RT | `D3D12_RAYTRACING_INSTANCE_DESC` | 64 (×2 slots) | rebuild da TLAS, todo frame com a câmera se movendo |
| HiZ | `FBoundsEntry` (center4 + extent4) | 32 | por objeto testado |
| RT | `FGpuInstanceGeo` | 20 | só em mudança estrutural |

São 480 B por instância reescritos por frame, contra 20 B que só mudam quando a cena muda de
estrutura. Esse é o número que a tabela de instâncias existe para atacar: uma instância parada não
deveria custar upload nenhum.

**Colunas propostas** (SoA, por padrão de acesso, seguindo §5.2 do plano):

| coluna | conteúdo | bytes | quem lê |
|---|---|---:|---|
| `InstanceMeta` | `MeshId`, `MaterialId`, `CellId`, `Flags`, `Generation` | 20 | culling, raster, RT |
| `InstanceBounds` | centro + extensão em espaço de MUNDO | 32 | culling (frustum, HZB), sombras, chuva |
| `Transform` | `Mat44` de mundo | 64 | raster, TLAS, temporal |
| `PrevTransform` | `Mat44` do frame anterior | 64 | apenas temporal e motion vectors |
| | **residente por instância** | **180** | |

Separar é o ponto: a thread de culling toca 52 B (meta + bounds) e não carrega duas matrizes para
testar uma esfera. `PrevTransform` fica em coluna própria porque só o temporal a lê, e porque ela é
a única que muda quando um objeto **estático** não se move — nesse caso não muda nada, que é
exatamente o dirty-row que interessa.

Na Emerald (2.479 renderáveis) isso dá **436 KB residentes**, contra os ~1,2 MB/frame que as quatro
formas atuais reescrevem.

**Por que não foi implementada nesta fase.** As colunas `Transform` e `Bounds` são as mesmas que o
`FTemporalMotionVectors` e o `FHiZOcclusion` já mantêm, cada um com o seu buffer e a sua indexação.
Criar a tabela sem migrar os dois produz uma **terceira** cópia dos mesmos dados — o oposto do que
a fase existe para fazer. Migrar os dois é mais do que a Fase 2 pede, e o consumidor que justifica
a tabela (culling na GPU) só nasce na Fase 6.

Decisão pendente do proprietário, com o mesmo peso da que resolveu o formato: implementar agora
absorvendo os dois sistemas, ou deixar a tabela nascer na Fase 4 já com o consumidor.

## Falta na fase

- `FGpuSceneInstance`: layout e orçamento **feitos** (seção acima); a implementação aguarda
  decisão do proprietário;
- medir bytes persistentes e sujos numa cena grande de verdade;
- comparação visual CPU/RT no mesmo regime da Fase 1 (preset scientific, `N=128`, 4 cenas × 2
  câmeras). **Exige GPU e aval do proprietário.**

## Risco anotado, ainda não medido

O caminho mais quente do RT (`AlphaTestPass`, que roda por CANDIDATO de travessia, não por hit
comitado) passou a fazer duas leituras dependentes onde fazia uma. A hipótese é que não seja perda:
o snapshot por instância vive num UPLOAD heap — memória do host, lida por PCIe em placa discreta —
e o que sai dele por hit caiu de 100 B para 20 B, enquanto a tabela de materiais vive em DEFAULT
heap e cabe inteira em poucos KB (214 materiais na Emerald = ~14 KB). Medir isso exige GPU.
