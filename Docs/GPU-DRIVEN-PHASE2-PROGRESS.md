# Fase 2 — estado da implementação

Data: 2026-09-04

Estado: **em andamento, não promovível.** As tabelas de materiais e de meshes estão entregues e
validadas em runtime; os quatro findings da revisão estão corrigidos. Faltam **duas decisões do
proprietário**, e são elas que seguram o encerramento da fase:

1. **`FGpuSceneInstance`** — desenhada e orçada, não implementada. Ver a seção do layout.
2. **Validação interativa** de três caminhos que não têm teste automatizado: a troca de textura de
   material (hazard de lifetime de GPU — quem prova é a debug layer numa sessão Debug), o slider de
   subsurface, e agora o rebake de micro-map. O rebake é observável: editar o corte de alpha numa
   folhagem e soltar tem de logar `[OMM] Rebake em runtime: N par(es)...` e fazer o painel mostrar
   BLAS refeitos, sem que o recorte da folhagem no GI divirja do raster. Vale exercitar também o
   caminho de **recusa** — trocar o BaseColor por uma textura gerada em runtime: o log tem de dizer
   qual peça faltou e o controle na UI tem de **voltar sozinho** ao valor anterior.

O gate do OMM deixou de ser pendência: **decidido pela reconstrução** e implementado.

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
| `cb6cab5` | os quatro findings da revisão de runtime |
| `1c1280f` | contratos `u32`/`u64` separados no MCP |
| `5063c84` | entrada compacta de geometria para o baker |
| `7e9c9a6` | rebake em runtime + reconstrução dos BLAS afetados |
| `ee39f68` | os quatro furos do caminho de reconstrução |
| `2877d82` | procedência que identifica bytes; recusa em vez de meia aplicação |
| (este) | fingerprint de conteúdo, atomicidade por material, ranges antes das alocações |

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

Os dois continuam sendo **bytes de conteúdo**, e não custo de alocação; a seção de telemetria
explica onde o custo real, com alinhamento, já é medido.

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
| OMM invalidado quando suas ENTRADAS mudam | **feito**: chaveado pelas quatro entradas, rebakeado no commit da edição e os BLAS afetados reconstruídos. Falta validação interativa |
| layouts e versões com testes de ABI, offsets e bounds | **feito** para material, mesh e instância; falta a de instâncias da Fase 4 |
| cena grande reporta bytes persistentes por tabela e sujos por frame | **feito e medido** na sintética de 1M: `rowsDirty=0` e `copyRegions=0` em regime estável. VRAM e staging são campos separados; ver "o que os bytes medem" |
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

### A reconstrução (decisão do proprietário, 2026-09-04)

Por um tempo as quatro edições que invalidam o micro-map foram **recusadas**, porque o runtime não
guardava com que rebakear. O proprietário optou por reconstruir, e é o que existe agora — as quatro
voltaram a ser edições de verdade.

**O que a cena retém: procedência, não dado.** Por malha, o par `{arquivo .smesh, índice}`; por
textura, o caminho mais as flags de decodificação do load original. Dezenas de bytes cada.

A primeira versão retinha os próprios UVs e índices, e tinha dois defeitos que a revisão pegou:
pagava memória residente por uma operação manual e rara, e — pior — só podia reter as malhas que já
eram **mascaradas no import**, então ligar Alpha Test num material opaco depois não tinha com que
bakear. Guardando procedência, cabe reter **todas** as malhas, e essa transição passa a funcionar
por construção em vez de por remendo.

O baker lê exatamente duas coisas: as UVs e os índices. Posição e normal nunca entram — o micro-map
é definido no espaço de textura do triângulo. Isso está provado, não suposto: o teste bakeia os
primeiros 8 pares mascarados da Bistro pelos dois caminhos e exige resultado **byte a byte
idêntico** (8, e não os 122, porque a propriedade sob teste é do encanamento da entrada, não do
conteúdo da malha — bakear todos duas vezes estourava o timeout do Debug).

O leitor sob demanda navega o layout `[header][entries][blob]` do `.smesh` à mão, e é o código mais
arriscado do rebake: um `sizeof` errado ali não daria crash, daria UVs de outra malha. Por isso ele
tem oráculo próprio — o teste lê **as 1591 malhas** da Bistro pelo leitor e exige igualdade com o
que o `SceneLoader` produziu. Deslocar a base do blob em 4 bytes derruba o teste.

**Quando dispara.** No **commit** da edição, nunca por tick. Arrastar o corte de alpha numa
folhagem dispararia um rebake por movimento do mouse; o `SliderRow` ganhou um sinal `committed`
(controle solto, ou passo discreto de teclado/roda) e é ele que publica.

E o tick tem de tocar **só a linha do material**. A primeira versão usava `TouchSelectedRT` por
tick, o que levanta `TlasFlagsDirty`: a TLAS era reconstruída no frame seguinte com o cutoff novo e
o micro-map ainda antigo, e o `CollectInstances` abortava — a render thread caía **antes** de o
slider ser solto. Rebake, flags de TLAS e invalidação de histórico saem juntos no commit, nessa
ordem, porque o rebake chama o `NotifyMaterialRTStateChanged` por último, depois de o micro-map
novo existir.

O valor continua acompanhando o arraste: o raster e o alpha-test dos raios leem a linha, que sobe
por tick. O que espera é o micro-map.

**Isso torna o RT transitoriamente inconsistente durante o arraste, e isso é aceito.** Enquanto o
micro-map não é refeito, o hardware classifica os microtriângulos pelo cutoff antigo e só os que
sobram como UNKNOWN chegam ao `RTAlphaTest`, que já usa o novo — então o recorte muda *em parte* no
RT. As duas alternativas custam mais que o artefato: manter o RT no valor antigo exigiria a linha
carregar um cutoff diferente do `FMaterial`, quebrando "a linha é função pura do material" — que é
o que torna a tabela testável e o `DiffMaterialRows` significativo; e segurar o raster junto
tiraria o feedback ao vivo, que é a razão de existir um slider. O artefato cabe dentro de um
arraste e não sobrevive ao commit.

**Os BLAS afetados.** `EnsureBlasFor` reconstrói apenas os pares `{malha, micro-map}` que passam a
ser exigidos, cada um num recurso próprio, e o `BlasByMesh` passa a apontar para lá. O espaço
antigo fica órfão dentro do `BlasPool` — e isso **não custa memória**: o pool já estava alocado no
tamanho dele. O que cresce são as alocações laterais, e é isso que `RebuiltBlasBytes` mede: o BLAS
**e o OMM Array que ele referencia**, porque os dois são acceleration structures alocadas por
rebake e contar só o primeiro subestimava justamente a metade que cresce com folhagem — que é onde
micro-map existe. Compactar o pool moveria todos os BLAS de endereço e obrigaria a refazer a TLAS
inteira: mais risco do que o desperdício justifica.

**`EnsureBlasFor` é transacional.** Nada é publicado — nem endereço no `BlasByMesh`, nem recurso,
nem contador — antes de a fila sincronizar. O invariante é *endereço registrado implica build
executada*, e publicar dentro do laço o quebrava: um par seguinte que lançasse (o driver recusa um
prebuild, uma alocação falha) deixava o primeiro endereço registrado com a command list **nunca
executada**, e o filtro do próximo `EnsureBlasFor` dava aquele par por pronto — a TLAS passaria a
apontar para memória de AS que ninguém construiu. `Close` e `ExecuteAndSync` também podem lançar, e
ficam do lado de fora da publicação pelo mesmo motivo.

**Os recursos substituídos são reclamados.** `ForgetBlas` tira a chave do `BlasByMesh` e move o
recurso do par para a lista de **aposentados**; a reconciliação seguinte os solta. A espera não é
por frames em voo — o chamador já drenou as duas filas —, é pela **TLAS**: ela guarda o endereço do
BLAS antigo até ser refeita, e o `RecordTlasRebuild` pode não ter acontecido (ele é condicional).
Por isso a reclamação exige o flag de rebuild já consumido; sujo, espera mais uma rodada. Segurar
VRAM por engano custa memória; soltá-la por engano custa a GPU. Aposentado ainda ocupa VRAM e por
isso continua somando em `RebuiltBlasBytes` até ser reclamado.

Isso fecha um crescimento que era permanente: antes, cada edição acrescentava um BLAS e um OMM
Array que só sumiam no próximo load, porque o vetor plano de recursos não tinha caminho de volta do
par para o recurso. Agora o registro é **por par**.

Uma travessia cobre as três transições que uma edição produz: micro-map rebakeado, alpha test
ligado (bake do zero) e alpha test desligado (passa a exigir o BLAS opaco daquela malha, que pode
nunca ter sido construído — e sem ele o `CollectInstances` derrubava a render thread).

A deduplicação por par vive **dentro** do `EnsureBlasFor`, e é correção e não otimização: a lista
chega com uma entrada por *renderável*, e ela é montada inteira antes de o primeiro resultado ser
publicado no mapa — então sem o conjunto, N instâncias da mesma folhagem construiriam N OMM arrays
e N BLAS idênticos. O custo escalaria por instância, exatamente o oposto do que a dedup do bake já
garante.

**Procedência é do conteúdo, não do caminho.** Caminho sozinho não identifica bytes: um recook
preserva magic e versão e pode trocar UVs, índices ou a ordem das malhas, e o rebake aceitaria em
silêncio — produzindo um micro-map que descreve geometria diferente da que está nos buffers de GPU.
A textura tem o mesmo TOCTOU. Cada origem carrega um **fingerprint do conteúdo** (FNV-1a por
palavra) tirado no import, recalculado e conferido a cada releitura.

Um carimbo `{tamanho, hora de escrita}` chegou a ser a resposta aqui e **não servia**, por uma
janela concreta: o carimbo só poderia ser tirado depois de o loader ler os bytes, então um recook
*nesse intervalo* deixaria os buffers antigos na GPU e registraria a identidade do arquivo **novo**
— a validação passaria exatamente no caso que ela existe para pegar. Fora isso, tamanho e hora
iguais com conteúdo diferente também passavam.

O fingerprint não custa I/O: ele sai dos bytes que o loader **já tem em RAM** — os mesmos que
viraram os buffers de GPU —, uma passada sobre dado residente. Duas consequências de forma:

- A geometria é percorrida **por elemento**, não por bytes crus: no import as UVs vivem dentro do
  `Vertex` (32 B) e na releitura vêm compactadas (8 B). Hash de bytes crus daria valores diferentes
  para a mesma geometria e a validação recusaria tudo — o modo de falha silencioso deste tipo de
  gate, e por isso as 1591 malhas do Bistro são conferidas contra o loader no teste.
- A textura hasheia o que o baker consome dela e nada mais: pixels do mip 0, dimensões, `Format` (o
  baker entrega os bytes crus ao DirectXTex) e `IsNormalMap` (que zera o alpha no decode). As flags
  de load **não são parâmetro** — saem do próprio dado —, senão daria para registrar um fingerprint
  que descreve algo diferente do que foi decodificado.

**Fingerprint ausente falha FECHADA.** `kOmmNoContentHash` é recusado no registro, não no consumo:
uma entrada sem fingerprint casaria com qualquer arquivo, e duas consultas com erro passariam a
"bater" uma com a outra — que era o defeito estrutural do carimbo.

**O leitor sob demanda valida os ranges antes de alocar.** `VertexCount` e `IndexCount` vêm do
arquivo e dimensionam os vetores; conferir depois de alocar significa um `.smesh` corrompido pedindo
dezenas de GB antes de qualquer checagem. Mesmo critério do `ArrayFits` do `SceneLoader`, com a
multiplicação checada contra overflow em vez de assumida.

**Quando o rebake não pode acontecer, a edição é DESFEITA**, não ignorada. Seguir com o micro-map
antigo deixaria material e micro-map descrevendo coisas diferentes — o estado híbrido que o
`CollectInstances` aborta no rebuild seguinte. Com micro-map existente, alpha/cutoff voltam ao que
ele descreve; sem micro-map (o alpha test acabou de ser ligado), o alpha test é desligado de novo,
porque sem OMM não há BLAS que sirva um material masked. Os dois casos avisam no log. A textura não
é revertida — o descritor dela já foi reescrito no heap —, e por isso ali a saída é desligar o
alpha test: visível e reversível pelo usuário, em vez de silenciosamente errado.

**A unidade de atomicidade é o MATERIAL, e a reconciliação roda em fases.** Levantar os pares stale
sem mutar nada, bakear todos, **decidir** o que a reversão fará sem fazê-la, montar o conjunto de
BLAS exigido pelo estado **final**, reconstruir, e só então publicar. Publicar dentro do laço de
bake — como a primeira versão fazia — deixava um material de duas malhas com a primeira já
publicada e o material revertido por baixo dela. Um material que falha em qualquer par não publica
par nenhum: os bakes que deram certo são descartados.

A reversão é **decisão** numa fase e **mutação** em outra, e a separação não é estilo. O conjunto de
BLAS exigido depende do estado final do material, então a decisão tem de vir antes da reconstrução;
mas mutar o `FMaterial` antes dela deixava o material já revertido com o micro-map antigo ainda
publicado no renderável, caso o `EnsureBlasFor` lançasse — o estado híbrido que o `CollectInstances`
aborta, com o dirty flag já consumido e ninguém para tentar de novo. A regra em si
(`FOpacityMicromapData::CanRestoreAlphaTestFor`) mora num lugar só, porque duas cópias divergem no
dia em que uma ganhar um caso a mais.

**Se a reconstrução dos BLAS falhar, a edição inteira é desfeita** — inclusive para os materiais
cujo bake deu certo. Como o `EnsureBlasFor` é transacional, o estado da GPU nesse ponto é o de antes
da edição, e o único estado de material consistente com ele é também o de antes: um bake pronto mas
sem BLAS descreve um micro-map que não existe.

Um caso que só aparece depois da reversão: material revertido **para opaco** larga o micro-map e
passa a exigir o BLAS opaco daquelas malhas — que pode nunca ter sido construído. Como esse par só
existe *por causa* da reversão, ele precisa entrar no `Required` depois dela e antes do
`EnsureBlasFor`; sem isso o `CollectInstances` cairia em "BLAS opaco ausente" no rebuild seguinte,
com o dirty flag já consumido — ninguém tentaria de novo.

**A UI descobre a reversão.** `RevertOmmInputs` incrementa um contador
(`GetMaterialRevertVersion()`) que o `MaterialsBridge::Refresh` compara por frame; quando ele anda,
os sinais de propriedade são reemitidos e os badges de todas as linhas recalculados. Um contador e
não um sinal porque o `Renderer` não conhece Qt, e a ponte já tem um tick por frame ligado ao
`FrameReady`. Sem isso o slider ou o checkbox continuariam exibindo o valor que acabou de ser
recusado — a UI mostrando um estado que a cena não tem.

**Textura carregada em runtime também retém procedência.** Trocar o BaseColor de um material
alpha-tested — o caso que motivou a reconstrução — criava uma textura sem procedência: o rebake
devolvia nulo, o micro-map antigo ficava e o `CollectInstances` abortava por chave desatualizada. O
`ImportRuntimeTexture` decodifica **uma vez** para CPU e usa esse mesmo dado para subir à GPU e para
registrar o fingerprint; reler o arquivo só para hashear reabriria a janela que o fingerprint existe
para fechar.

**O que ainda não é reconstruível**, e falha alto dizendo qual peça faltou: malha criada em runtime
(sem procedência) e material cujo BaseColor foi gerado em vez de vir de arquivo — o proxy do
terreno é o caso real. Nenhum dos dois é alpha-tested hoje.

### Cobertura do caminho de reconstrução

`Smile.OpacityMicromapBake.BistroExterior` cobre a procedência inteira, e cada guarda foi provada
**negativamente** — a regressão injetada, o teste vermelho, a injeção desfeita:

| guarda | prova negativa |
|---|---|
| ranges antes das alocações | lambda `RangeFits` neutralizada → o teste **morre** (`0xC0000409`) na entrada com `VertexCount = 0xFFFFFFFF`, que é exatamente o ponto: sem a checagem o vetor de ~137 GB é construído antes de qualquer validação |
| fingerprint da geometria | comparação desligada no `LoadGeometry` → `fingerprint divergente foi aceito pelo leitor` |
| fingerprint da textura | comparação desligada no `LoadAlphaTexture` → `textura com conteudo divergente foi aceita` (o teste registra o caminho de uma textura com o dado de outra, que é o que um recook produz) |
| fail-closed no registro | `kOmmNoContentHash` aceito no `RetainMeshSource` → `registro sem fingerprint foi aceito` |
| hash por elemento | stride assumido compacto → `LoadGeometry falhou` nas **1591** malhas, que é o modo de falha silencioso que essa forma evita |

O que **não** tem prova automatizada, e continua sendo o mesmo conjunto de sempre: a atomicidade por
material, a reversão para opaco exigindo o BLAS opaco e a reemissão dos sinais na ponte Qt. Os três
são de sessão de editor com GPU. O rebake é observável no log
(`[OMM] Rebake em runtime: N par(es)...`) e no painel de BLAS refeitos.

## Telemetria

`FCpuFrameStats` publica, por tabela: linhas em uso, bytes de conteúdo em VRAM, bytes de staging no
host e — só para materiais — linhas sujas neste frame, regiões de cópia e o acumulado de linhas
subidas. Chegam ao painel (`Tabelas da cena`) e ao snapshot MCP (`cpu.materialTable`,
`cpu.meshTable`).

O protocolo está em **5**. Ele subiu porque o cliente TypeScript passou a **exigir** e a **resumir**
os dois blocos — a regra da v4 vale aqui: a versão sobe quando o cliente exige, não quando o editor
publica. Antes disso os blocos existiam só na amostra crua, e como o `includeSamples` default é
`false`, a régua nunca os via.

**Dois contratos de tipo, e não um.** Contagens (`rows`, `rowsDirty`, `copyRegions`) são `u32` no
produtor. Bytes e `rowsUploadedTotal` são `u64` — o acumulado soma desde o último setup da tabela e
não volta a zero por frame, então dar a ele o teto de `u32` recusaria um snapshot legítimo de um
editor aberto há muito tempo. O teto do `u64` na borda é `Number.MAX_SAFE_INTEGER`, e não 2^64: o
editor serializa como `double`, então acima de 2^53−1 o número já chega perdendo precisão.

`rowsDirty` é o número do gate: zero em todo frame sem edição de material. O acumulado existe
porque o instantâneo é quase sempre zero — um vazamento de um punhado de linhas por frame não
aparece no instantâneo e aparece na soma.

### O que os bytes medem, exatamente

`residentBytes` e `stagingBytes` são **bytes de conteúdo**: capacidade × stride, e a capacidade ×
stride × frames em voo. É a grandeza que responde "como a tabela escala com a cena", que é para o
que o gate existe.

Eles **não** são o custo de alocação. O D3D12 arredonda um committed resource para a granularidade
de heap, então uma tabela de 3.136 B ocupa muito mais que isso na prática — e nas cenas atuais, em
que as tabelas são de poucos KB, o arredondamento domina.

Isso já é medido, em outro lugar: o `VramTracker` registra o **tamanho real com alinhamento**
(`VramTracker.cpp`, "igual ao que o driver cobra") e as duas tabelas entram no breakdown por
recurso com rótulo próprio, `GpuMaterialTable` e `GpuMeshTable`. O staging não aparece lá de
propósito — é UPLOAD heap, que o tracker ignora por ser memória do host —, e é justamente por isso
que ele precisa de campo próprio aqui.

Ou seja: para "quanto a tabela custa de verdade", o breakdown de VRAM; para "como ela cresce com a
cena", estes dois campos.

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
