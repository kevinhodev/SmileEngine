# Fase 4 — estado da implementação

Data: 2026-09-10

Estado: **aberta, oito consumidores migrados e três rodadas de revisão fechadas.** Release fecha
66/66, Debug compila e o cliente TypeScript compila. Três dos seis itens do gate estão fechados; o
`ObjectCB` **já pode deixar de ser alocado** (sete consumidores migrados, alocação sob demanda,
troca transacional), e a aposentadoria do `TransformBuffer` do TemporalMotion começou pela
matemática. Falta a medição em 1M, que é o que fecha os dois.

A primeira revisão do Codex reprovou a fase com seis achados, três bloqueadores — entre eles uma
regressão de motion vector que eu tinha introduzido e um trio de checagens que eu havia
**documentado como decisão deliberada**. A segunda não achou P1 novo e pegou um defeito no aparato
de teste. A terceira, sobre o `ObjectCB` sob demanda, achou dois P1, um P2 **e uma afirmação de
estado errada minha** — eu disse que faltavam "1M + documentação" com o `TransformBuffer` intacto.
Todos corrigidos; as seções de revisão registram cada um e o que o padrão deles ensina.

Base: Fase 3 encerrada em `3b38694`. **Nada da Fase 3 foi reaberto.**

O default não mudou. Nenhum consumidor de frame lê a lista de candidatos ainda — ela é contrato,
não caminho quente.

## O que foi entregue

| commit | slice |
|---|---|
| `a7fa48c` | descobrir o que mudou custa o que mudou (produtor + diff incrementais) |
| `58cf601` | os caminhos quentes do editor dizem quem mudou |
| `d190e10` | `FInstanceRange` e a lista de candidatos |
| `d6cd442` | expansão de ranges na GPU, com validação e contadores |
| `24c5194` | índice repetido na caixa de correio destruía o `PrevTransform` |
| `3fb93cd` | o candidato carrega a generation, e o overflow deixa de ser parcial |
| `80b9dbf` | isolar material não invalidava o mapa estático de sombra |
| `f67d646` | o aparelho de teste alocava dois slots por renderável |
| `0704793` | a composição do transform sai do CBV por draw (composição compartilhada C++/HLSL) |
| `b82c92b` | caminho bindless de transform selecionável no main view (z-prepass + G-buffer) |
| `8679e01` | MCP expõe o toggle e os contadores de transform (protocolo 10) |
| `7c30c6a` | sombras do sol lendo o transform da tabela |
| `e0d0ece` | MCP declara os pré-requisitos da leitura de sombra (protocolo 11) |
| `bb572c0` | sombras locais lendo o transform da tabela |
| `66b91ad` | mapa de oclusão de chuva lendo o transform da tabela |
| `e56cb4f` | translúcidos lendo o transform da tabela |
| `b6cef2f` | picking lendo o transform da tabela |
| `6055d6a` | A/B do picking no teste versionado — **falso-verde**, ver a revisão |
| `13b2b51` | fecha o falso-verde do A/B do picking |
| `0e6688e` | `ObjectCB` alocado sob demanda, com troca transacional |
| `e0841b5` | liberar o `ObjectCB` exige o caminho clássico inteiro aposentado |
| `092e0c0` | alocação do `ObjectCB` que recusa de verdade (`TryCreateUploadBuffer`) |
| `f4f84c5` | MCP valida os dois números do `ObjectCB` na borda (protocolo 12) |
| `d28cb52` | derivação do par temporal, com paridade provada em WARP |

### Os sete consumidores do `ObjectCB`

Cada um foi migrado, medido pelo par `transformBinds`/`transformSlotSets` e aprovado em GPU antes do
seguinte. A ordem não foi arbitrária: do mais visível para o mais silencioso.

| passe | evidência da rodada |
|---|---|
| z-prepass opaco + mascarado | Z opaco 572.373, Z masked 191.035 — slots == draws exatamente |
| G-buffer | 763.408; OFF↔ON dentro do ruído de ON↔ON |
| sombras do sol | 10/10 amostras redesenharam 2.678 casters; OFF binds 2.678/slots 0, ON binds 0/slots 2.678 |
| sombras locais | 20/20 amostras, 5.880 considerados, 1.186 draws; controle negativo com `castShadows:false` |
| oclusão de chuva | câmera forçada a cruzar o quantum de 16 m dez vezes por caminho; 1.591 considerados |
| translúcidos | ~890 considerados, 27–29 draws; vidro intacto, p95 ≤ 1/255 |
| picking | A/B no teste versionado, em hardware Tier 3 — o único cuja falha não aparece na imagem |

## O gate de saída, item a item

| # | Gate | Estado |
|---|---|---|
| 1 | cena estática envia zero dirty pages depois do assentamento | **fechado** — já valia pela saída antecipada por versão; agora está sob teste (`Smile.GpuSceneIncremental`) |
| 2 | mover um objeto não faz upload proporcional ao total | **fechado** — e o custo de *descobrir* também deixou de ser |
| 3 | remover/reusar slot respeita generation e fences | **fechado** — herdado da Fase 1; o teste novo cobre a linha zerada na remoção |
| 4 | 1M instâncias não exigem reconstrução/upload integral por frame | **provado em 4.096, não medido em 1M.** Falta a medição na sintética |
| 5 | `ObjectCB` e `TransformBuffer` do TemporalMotion deixaram de ser alocados | **metade pronta, nenhuma medida.** `ObjectCB`: sete consumidores migrados e alocação sob demanda — com o toggle ligado ele não existe. `TransformBuffer`: só a matemática (`d28cb52`); o buffer continua alocado e reescrito por frame |
| 6 | range único e múltiplos ranges sintéticos produzem a mesma lista expandida | **fechado** — na CPU e na GPU |

## Revisão do Codex (2026-09-08) — seis achados, três bloqueadores

A revisão do range `3b38694..c28038a` reprovou a fase. Os seis achados eram defeitos de verdade;
nenhum era cosmético, e três deles eu tinha **documentado como decisão deliberada** — o que os
tornava piores, não melhores.

### P1 — índice repetido destruía o `PrevTransform`

Regressão minha, de `a7fa48c` + `58cf601`. Mover um objeto **estático** bumpa transform *e* static
casters, e desde a segunda slice os dois marcam o mesmo renderável: a caixa chega com o índice
duas vezes. É o caso comum — campo de transform do Outliner, `set_object_position` do MCP —, não
uma borda.

A segunda escrita da mesma linha não era redundante, era destrutiva:

```
WriteRow #1: Current=B, Previous=TransformRows_[S]=A  → Transform=B, Prev=A   ✓
WriteRow #2: Current=B, Previous=TransformRows_[S]=B  → Transform=B, Prev=B   ✗
```

O objeto perdia o passado no exato frame em que se movia: motion vector zero, sem erro nenhum, só o
objeto deixando de borrar. A deduplicação que existia acontecia em `TouchedSlots_`, **depois** das
escritas — tarde demais.

Conserto: uma escrita por slot por ciclo, pelo epoch que o `PrepareFrame` já mantém.

O teste de duplicatas que eu tinha escrito **não cobria isto**: ele ultrapassava o teto de 4.096 e
caía na varredura completa, que escreve cada slot uma vez. O caso novo repete o índice **abaixo** do
teto e confere as **matrizes**, não os contadores — uma deduplicação tardia deixaria os contadores
certos e o previous errado.

### P1 — a validação de generation não existia

A lista carregava só `u32 Slot`, então o shader só conseguia perguntar "esta linha tem **algum**
ocupante?". Um candidato de uma lista atrasada aceitaria em silêncio o ocupante **novo** de um slot
reciclado: objeto errado, transform errado, sem contador subindo.

O argumento que eu tinha escrito para não carregar a generation — reciclar slot passa por
`Add`/`Remove`, que bumpam a versão de estrutura e reconstroem a lista — continua verdadeiro, e
continua sendo o **motivo errado**: o valor de uma checagem está em verificar o invariante, não em
presumi-lo. Custa 4 B por candidato.

`FGpuCandidateId {Slot, Generation}`, com espelho HLSL. A generation **zero** do candidato é
recusada antes da comparação: sem isso, um candidato inválido (generation 0) apontando para uma
linha vazia (generation 0 também) casaria — dois erros se cancelando num acerto aparente.

### P1 — overflow produzia lista parcial utilizável

Contar o descarte não o torna conservador. Uma lista truncada e consumível é geometria sumindo em
silêncio, que a regra 6 do plano proíbe — e os testes antigos **institucionalizavam** isso,
esperando dois candidatos e três sumidos como comportamento correto.

Agora: ou a lista descreve a cena inteira, ou ela não existe. Com overflow ela vem **vazia** e
`IsUsable()` é falso; o consumidor cai no caminho CPU (D9). Lista vazia com a flag em falso falha
**alto**; três objetos faltando falha baixo, e é a baixa que atravessa uma revisão.

`Stats().Candidates` passou a contar a cena inteira, e não o que coube — é com esse número que se
redimensiona o buffer. Contando só os que couberam, a capacidade necessária nunca seria observável e
o overflow seria insolúvel.

No shader o overflow virou **canário** explícito: um dispatch não tem como cair no fallback, então
quem impede a saída truncada é o dimensionamento pela CPU. Diferente de zero significa "a lista
deste frame não pode ser consumida".

### P2 — a GPU lia fora do buffer onde a CPU recusava

O shader não recebia o tamanho de `CandidateIds`, então o mesmo range que a referência ignora
produzia aqui leitura fora do `StructuredBuffer`. Ela **não trava** — o D3D12 devolve zero —, e por
isso é pior: o candidato vira o slot 0, que é plausível, e a GPU passa a discordar da referência em
silêncio. A guarda compara por subtração no lado seguro, porque a soma pode dar a volta em 32 bits.

### P2 — mudar a capacidade não invalidava a lista

`SetCapacity` só trocava o membro, e `RefreshIfStale` olha versões da cena. Aumentar o teto depois de
um overflow deixava a lista truncada para sempre. Agora invalida quando o valor **muda** —
reatribuir o mesmo valor não reconstrói, senão um chamador por frame reconstruiria por frame.

### P2 — isolar material não invalidava a membresia

Defeito **pré-existente**, anterior à Fase 4. `ApplyIsolation`/`StopIsolation` escondem e mostram
renderáveis e bumpavam só `TransformsVersion`. Ocultar ou mostrar muda o que o mapa estático contém
— o olho do Outliner já bumpa as duas por essa exata razão, com o comentário escrito lá desde antes.

Consequência de hoje: o shadow map cacheado continua com os objetos escondidos dentro dele enquanto
o isolar está ligado. Consequência da Fase 4: a lista de candidatos ficaria com a membresia velha.

Auditei os outros seis escritores de `FRenderable::Visible`. Os cinco restantes estão certos: o olho
do Outliner, a lista de ocultas do JSON e o `.smap` já bumpam as duas; os dois de terreno tocam
**apenas** proxies `RaytracingOnly`, que estão fora do raster e do CSM de qualquer forma.

### Segunda rodada — P2 no aparato de teste

Nenhum P1 novo. Sobrou um defeito no **aparelho**, e ele vale ser registrado porque a categoria é
traiçoeira: os dois testes sobrescreviam o handle que o `AddRenderable` já havia alocado com um
segundo `GpuSlots().Allocate()`, deixando o primeiro slot `Live` e sem dono.

O estrago não era só contábil. `SetupForScene` dimensiona a tabela por `GpuSlots().Capacity()`, então
a folga que o teste conferia vinha do **vazamento** e não do `TableCapacityFor` — a asserção "a
capacidade tem folga sobre as linhas vivas" passava por acidente.

E o teste incremental aposentava o handle na mão antes do `RemoveRenderable`. `Retire` é idempotente,
então isso não corrompia contador nenhum — e era exatamente esse o problema: o teste passaria
idêntico se o `RemoveRenderable` parasse de aposentar a residência. Confirmado pelo caso negativo:
sem o `Retire` manual, remover a aposentadoria da produção deixa 4.096 vivos e zero aposentados, e o
teste fica vermelho.

Regra que sai daí, irmã da anterior: **um passo de setup que contorna a API de produção compra
conveniência com cobertura.**

### O padrão dos três primeiros

Os três P1 tinham a mesma forma: um comentário meu explicando por que o caminho mais fraco bastava.
Nos três casos o raciocínio estava certo **sobre o código de hoje** e errado sobre o que uma
checagem serve para fazer. Vale como regra para as fases seguintes: quando o comentário explica por
que **não** verificar, o comentário é o defeito.

## Revisão do Codex (2026-09-10) — o `ObjectCB` sob demanda

Dois P1, um P2 e uma afirmação de estado errada minha.

### P1 — o `ObjectCB` podia ser liberado com passes ainda no clássico

`SetBindlessTransforms(true)` não exigia `UseBindlessMaterials`. Como as PSOs de transform são
gêmeas das de material, com o eixo de material **desligado** todo passe que consome material escolhe
a PSO clássica — e a clássica lê o CBV por draw, que o toggle acabara de liberar. Reproduzido
ligando transform antes de material, e também desligando material pela UI com transform ligado.

O comentário no setter dizia que a dependência existia. Ela existia **na construção das PSOs**, não
no estado dos toggles: `BindlessTransformsReady()` responde "as PSOs existem?", e ninguém perguntava
"os passes vão usá-las?".

Segundo defeito no mesmo achado: a lista de prontidão cobria as seis PSOs do main view. A do
`ForwardBlend` (translúcidos) ficou de fora quando o passe foi migrado, e nada notou.

Correção em `e0841b5`: a decisão passa a sair de **um** predicado, `CanRetireObjectCB`, chamado pelo
setter e pelo `RecreateObjectCB`. A condição de LIBERAR é a conjunção literal do que cada passe
avalia para NÃO USAR — eixo de material ligado, as duas tabelas construídas, o CBV de view e as PSOs
das **sete** consumidoras. Desligar material passa a cascatear para transform (o outro sentido do
invariante, que a UI produz com um clique) e recusa a operação inteira se o `ObjectCB` não puder ser
recriado. Desligar transform sozinho continua legal: material bindless com transform clássico é um
degrau da escada do A/B.

Sobra uma rede no único ponto do frame que lê o endereço do buffer: se algum passe ainda alcança o
clássico e o buffer sumiu, ele é recriado ali, sem dreno — ninguém pode estar lendo o que não
existe. O silêncio era o caro: **nem todo consumidor é visível.** Sem buffer, o picking devolve outro
objeto e a oclusão de chuva perde o telhado, e a imagem não muda.

### P1 — a falha de alocação não era transacional

`AllocateObjectCB` prometia "devolve `false` sem tocar em nada", mas chamava `CreateUploadBuffer`,
que usa `SMILE_HR` na criação **e** no `Map`. A falha nunca chegava ao `if (!Upload.Resource)`: ela
saía como exceção pelo setter do Qt/MCP. A troca transacional só era transacional no comentário.

Correção em `092e0c0`: entra um `TryCreateUploadBuffer` real, com o corpo compartilhado com o que
lança — uma única função devolvendo `HRESULT`, para as duas versões não poderem divergir. Ela nunca
entrega um buffer meio pronto: se o `Map` falha, o recurso morre com o `ComPtr` local.

Os dois chamadores divergem **de propósito**: o setter recusa e preserva o bindless (que funciona);
o `RecreateObjectCB` (load/setup) **aborta**, porque ali não há estado anterior para preservar e
seguir sem buffer entregaria um renderer que não desenha e não reclama.

### P2 — o protocolo 12 não validava o campo que passou a exigir

`objectConstantBytesReserved` entrou no tipo e no resumo sem entrar na validação do
`profile_snapshot`. Apagar o campo do payload atravessava a borda, e o resumo publicaria `undefined`
exatamente no número que decide a fase — a mesma falha que fez o `gpuSlots` virar exigência na v4.

Correção em `f4f84c5`: os dois campos validados como u64 (`isSafeInteger`, não negativo) — e não
u32, porque em 1M o reservado passa de 512 MB — mais uma coerência entre eles: **escrita não pode
passar do reservado**. O caso extremo, `bytes > 0` com `reserved == 0`, descreve um renderer
impossível, e é justamente o par que as duas provas do gate existem para manter independente.

### E a afirmação de estado errada

Eu disse que faltavam "1M + documentação". O `TransformBuffer` do TemporalMotion continuava
alocando `2 × capacidade × 128 B`, criando os SRVs e sendo reescrito inteiro por frame — 256 B
residentes por instância, ~256 MB em 1M, e o gate exige explicitamente que ele desapareça.

Vale como regra: **um item de gate não sai da lista porque o outro item do mesmo bullet andou.**

### O falso-verde do A/B do picking (`6055d6a` → `13b2b51`)

Merece registro próprio porque o defeito estava no **teste**, e ele passava. O caso de escala e
rotação não uniformes: `HitY` era sobrescrito no ramo da borda, `SkewedClassic` nunca era comparado
com `HitId` nem com zero, a borda era opcional ("se achar") e os pixels ditos adjacentes estavam a 8
de distância — uma divergência de fração de pixel cabe inteira entre eles. O revisor provou o
vermelho numa 3060 Ti acrescentando um único `Require`.

Três rodadas seguidas com defeito no meu **aparato de teste**, e não na implementação. A regra que
sai daí: **código de teste merece a mesma desconfiança que código de produção** — e um `Require` que
nunca falha em nenhum estado é indistinguível de nenhum `Require`.

## O achado que definiu a primeira slice

As linhas sujas já eram poucas — o gate da Fase 2 provou isso. O que ninguém media era o **custo de
descobri-las**, e ele era a tabela inteira, duas vezes:

- `PrepareFrame` revarria todos os renderáveis a cada bump de versão. Num arraste de gizmo, por
  frame;
- `RecordUpload` chamava quatro `DiffTableRows` sobre vetores dimensionados pela **capacidade**. Na
  sintética de 1M são ~191 MB de `memcmp` por frame para achar uma linha.

E `rowsDirty` valia 1 nos dois casos. O contador que existia media o resultado, não o trabalho.

O conserto tem duas metades independentes, e nenhuma delas basta sozinha:

1. **o diff** passa a visitar só o conjunto que o produtor acabou de escrever. O produtor já sabia
   esse conjunto por construção e o jogava fora;
2. **o produtor** ganha uma caixa de correio na `FScene`. `BumpTransformsVersion(indice)` e
   `BumpStaticCastersVersion(indice)` dizem quem mudou; as versões sem argumento continuam válidas
   e **invalidam** a caixa, jogando o produtor na varredura completa.

A direção do erro é o ponto do desenho: não há como bumpar sem uma das duas coisas acontecer, então
esquecer o índice custa trabalho a mais e nunca uma linha desatualizada. Mudança de estrutura
invalida a caixa dentro da própria `FScene`, porque ela guarda índice e remover desloca a cauda.

O assentamento do `PrevTransform` ganhou lista própria pelo mesmo motivo: sem ela, o frame seguinte
a um movimento revarreria a cena só para descobrir quem ficou para trás — devolvendo pelo
assentamento o custo que a caixa tirou.

### O contador não podia ser proxy

A primeira versão de `rowsScanned` valia `TouchedSlots_.size()`. Um diff revertido para a
capacidade inteira continuaria reportando 1 — o contador mediria a intenção, não o trabalho. Ele
passou a contar **de dentro do laço**, uma unidade por (slot, coluna). O caso negativo confirma:
36.864 em vez de 4.

### Quem recebeu a dica, e quem não recebeu

Recebem: arraste de gizmo (o caso que motivou tudo, um bump por frame enquanto o gesto dura),
início e fim do arraste, campo de transform do Outliner, toggle de mobilidade, `set_object_position`
do MCP e os proxies `RaytracingOnly` do toggle de terreno.

Não recebem, e estão certos assim: aplicação de `.smap`, o olho do Outliner sobre uma subárvore e a
lista de ocultas do JSON tocam muitos objetos de uma vez, e para eles a varredura sai mais barata
que a lista. Edição de material também continua cega — mudar o conteúdo de um material não mexe em
nenhuma linha de instância, então o diff acha zero de qualquer jeito.

## Correção de passagem

As colunas CPU da tabela eram dimensionadas pela capacidade **recém-calculada**, enquanto o
`Capacity()` público — o mesmo que o produtor usa como limite de slot — é o do **buffer**, que o
`Create` preserva maior quando a cena nova é menor. Hoje as duas coincidem porque o pool só cresce;
um slot entre elas passaria pela guarda e indexaria fora do vetor.

## O contrato de candidatos

O range endereça um span da **lista**, e nunca um intervalo de slots. Um `{FirstSlot, SlotCount}` só
descreveria uma célula se a alocação de slots fosse particionada pela hierarquia, e isso conflita
com a free list global da Fase 1. Não existe modo "range identidade" — uma flag que pulasse a lista
faria `FirstCandidate` voltar a ser lido como slot.

`FRenderable::RaytracingOnly` fica de fora, e isso é **exclusão**, não fallback. Sem herdar o filtro
que o laço CPU já aplica, a expansão desenharia o chão duas vezes — e duas cópias coincidentes do
mesmo chão não aparecem numa comparação visual estática.

A lista é **persistente**: só se reconstrói quando a membresia pode ter mudado (estrutura e static
casters, que carrega visibilidade e mobilidade). Transform fica de fora de propósito.

Contadores separados por causa, e não somados: `RaytracingOnly` crescendo é normal, `InvalidSlot`
crescendo é defeito de residência, e `Overflow` diferente de zero é geometria que some — o que a
regra 6 do plano proíbe.

## A expansão na GPU

Uma thread por **saída**, não por range: a cena plana emite exatamente um range, e uma thread por
range serializaria tudo. A thread acha seu range por busca binária sobre o prefixo exclusivo das
contagens, calculado na CPU — com um range, um dispatch de prefixo custaria mais que a cópia.

Escrita **posicional**, não append atômico. Esta fase não rejeita ninguém, então a expansão é 1:1 e
a posição já é conhecida; isso deixa a lista da GPU byte a byte comparável com a referência CPU, que
é o gate 6. Um append devolveria a mesma lista embaralhada e o gate viraria comparação de
multiconjunto — mais fraco, e por nada, porque compactar só ganha sentido com rejeição de verdade
(Fase 6).

Toda posição recebe sentinela antes de qualquer saída antecipada: uma posição não escrita carregaria
o slot do frame anterior, que é um candidato plausível e errado.

### O que a validação de generation prova, e o que não prova

**Prova:** o slot cabe na tabela e a linha tem ocupante (generation 0 é reservada para linha vazia).

**Não prova:** que o ocupante é o mesmo de quando a lista foi montada — para isso a lista teria de
carregar a generation esperada. Hoje isso é coberto **por construção**: todo evento que recicla slot
passa por `Add`/`Remove`, que bumpam a versão de estrutura, que reconstrói a lista. Se essa
invalidação algum dia deixar de valer, carregar a generation na lista é a saída documentada.

O contador de `RaytracingOnly` alcançado é verificação **cruzada** da exclusão que o produtor CPU
aplica. Ele existe para o dia em que o produtor parar de filtrar.

## Achado de aparelho (WARP)

Um buffer de 1 byte com um SRV de um elemento de 8 B **não falha na criação**. Ele desfaz o device
no dispatch seguinte, e a falha aparece como `DEVICE_REMOVED` numa criação de recurso sem relação
nenhuma — a causa fica a cinco chamadas de distância. O aparelho do teste passou a ter piso por
**stride** e a imprimir o motivo da remoção junto da falha.

## Testes novos

| teste | o que prova |
|---|---|
| `Smile.GpuSceneIncremental` (WARP) | primeira publicação, assentamento, mover um objeto, storm de 1/8 da cena, remoção, bump cego, dica invalidada no mesmo intervalo, estouro do teto da caixa |
| `Smile.GpuSceneCandidates` (WARP) | as cinco causas de exclusão por contador **e** por identidade do slot, range único, equivalência com quatro ranges sintéticos, range estourado, range que dá a volta em u32, persistência sob transform, reconstrução sob visibilidade, overflow com teto |
| `Smile.GpuSceneExpandWarp` (WARP) | o shader de produção contra a referência CPU; range único vs sintéticos; slot fora da tabela; linha vazia; linha `RaytracingOnly`; overflow; lista vazia; os quatro índices de contador lidos de volta |
| `Smile.InstanceTransformWarp` (WARP) | a composição das quatro matrizes do VS, C++ contra HLSL, pelo include de produção — a ordem do produto é o erro clássico da migração |
| `Smile.TemporalTransformWarp` (WARP) | o par temporal derivado: inversa 4x4 e ordem do produto, com caso espelhado, longe da origem, sem histórico, singular e **não afim**, mais round-trip para a identidade |
| `Smile.RendererIdentity` (hardware, opt-in) | ganhou o A/B do picking bindless e a transação inteira do `ObjectCB`: invariante transform⇒material nos dois sentidos, `reserved == 0` com o toggle ligado, `> 0` na volta, e a recusa por falta de memória |

## Casos negativos rodados

Cada propriedade foi verificada **reintroduzindo a regressão** e confirmando o vermelho, antes de
declarar feito:

| regressão reintroduzida | resultado |
|---|---|
| `FullScan = true` sempre (produtor volta a varrer) | 16.384 em vez de 4 — vermelho |
| diff sobre a capacidade inteira | 36.864 em vez de 4 — vermelho |
| filtro `RaytracingOnly` removido do produtor | 9 asserts vermelhos, entre eles o slot do proxy na lista |
| range lido como intervalo de slots (CPU) | vermelho na comparação com os slots esperados |
| range lido como intervalo de slots (GPU) | 13 asserts vermelhos |
| verificação cruzada de `RaytracingOnly` removida do shader | 3 asserts vermelhos |
| guarda de epoch removido do `WriteRow` | vermelho no previous colapsado em current |
| generation de volta a "a linha tem ocupante?" | 3 asserts vermelhos |
| guarda de limite da lista removida do shader | 3 asserts vermelhos, com dois slots lidos além da lista entrando como válidos |
| alocação dupla de slot no aparelho de teste | 8.192 vivos para 4.096 renderáveis — vermelho |
| `RemoveRenderable` sem aposentar a residência | 4.096 vivos e zero aposentados após a remoção — vermelho |
| eixo de material fora do `CanRetireObjectCB` | "transform bindless ligou sem material bindless" — vermelho |
| cascata material→transform removida | "desligar material deixou o transform bindless ligado" — vermelho |
| `CreateUploadBuffer` de volta no lugar do `Try` | `HRESULT 0x8007000E` atravessando o setter — vermelho |
| validação dos dois campos de bytes removida do bridge | 11 dos 14 payloads de borda passam a ser aceitos; os 3 restantes caem na coerência, um deles citando o campo errado |
| ordem do produto trocada no par temporal | vermelho em 3 dos 6 casos (os outros são simétricos por construção) |
| sinal de cofator trocado na inversa HLSL | vermelho — **e verde antes de o conjunto de teste ser consertado** |
| corte de determinante removido da inversa HLSL | "a inversa singular não produziu NaN/Inf" — vermelho |

Um segundo achado do processo, na mesma linha do primeiro: o sinal de cofator trocado passava
**verde** enquanto as matrizes de teste usavam rotação só em Y. Uma rotação de eixo único deixa
zeros estruturais no 3x3, e o termo trocado multiplicava zero. Com entrada afim — a única que um
transform de objeto produz — metade dos termos de cada cofator é inalcançável por construção, e por
isso o conjunto ganhou um caso **não afim**: a função sob teste é uma inversa geral, e o que se
prova é a transcrição dela, não o uso.

Um achado do processo: a equivalência "range único vs sintéticos" **não** pega o range lido como
intervalo de slots — os dois modos cobrem o mesmo span, então a interpretação errada produz a mesma
saída nos dois. Quem pega é a comparação com os slots esperados, e por isso o teste ganhou uma
guarda explícita de que a lista de candidatos **não** é a identidade.

## Protocolo do MCP

Foi a **12**, em três degraus:

- **10** — `transformBinds`/`transformSlotSets` por passe e o toggle `bindlessTransforms`. Os dois
  contadores entram no resumo porque `transformBinds == 0` é a prova de **qual caminho executou**, e
  a imagem sozinha não dá isso: os dois caminhos produzem o mesmo pixel.
- **11** — `sunShadows` e `shadowCascadeCache` com readback efetivo e override, mais
  `bindlessTransforms` no manifesto da captura. São pré-requisitos de **leitura**: com o cache das
  cascatas ligado, `sunShadows.draws == 0` é ambíguo entre "não desenhou" e "reusou o mapa".
- **12** — `objectConstantBytesReserved`. As duas provas do gate, separadas: `objectConstantBytes ==
  0` diz que ninguém mais **escreve**; esta diz que o buffer deixou de **existir**. Durante seis das
  sete migrações a primeira caiu por passe enquanto a segunda ficou intacta.

O número de memória sai do **renderer**, e não do VramTracker: upload heap não é itemizado lá (o
tracker cobre VRAM/DEFAULT). Procurar a ausência do `ObjectCB` no breakdown por recurso não acharia
nada, nem antes nem depois.

## O que falta

1. **Medir na sintética de 1M.** Fecha os gates 4 e 5 de uma vez. Com
   `bindlessMaterials + bindlessTransforms` ligados: `objectConstantBytes == 0` **e**
   `objectConstantBytesReserved == 0`, com `transformBinds == 0` nos sete passes. No clássico, o
   esperado é ~256 MB escritos por frame contra ~563 MB reservados — são dois frames em voo sobre a
   capacidade com folga, e a razão entre os dois números é a própria evidência de que as duas provas
   medem coisas diferentes. **Exige GPU.**
2. **Aposentar o `TransformBuffer` do TemporalMotion.** Começou em `d28cb52` pela matemática; falta
   migrar os consumidores e apagar o buffer. Detalhe abaixo.
3. **Ligar a lista de candidatos ao frame** e publicar os contadores dela. Hoje
   `FGpuSceneCandidates` não tem chamador; o consumidor natural chega com a Fase 5.
4. **Exercitar a troca de toggle com cena carregada**, nas duas ordens, e capturar depois. A
   transação drena e realoca com o `MaxObjects` corrente; um erro ali aparece como raster vazio
   **depois de desligar**, não ao ligar. **Exige GPU.**

### O plano do `TransformBuffer`, slice a slice

O que ele guarda (`Current⁻¹·Previous` e `Previous⁻¹·Current`) é **derivado** de duas matrizes que
já moram na tabela canônica — e as entradas saem literalmente de lá, via `TransformsFor`. O custo
não é só a memória: o `UpdatePerFrame` faz **duas inversas 4x4 por instância por frame**, na CPU, em
cima da cena inteira. Em 1M isso é 2M inversas e 128 MB de escrita por frame.

A tensão anotada antes — "a tabela existe para evitar inversão por pixel, e os consumidores indexam
por `InstanceID`, não por slot" — se resolve por um caminho que já existe: os três consumidores já
leem a **tabela de identidade**, que é indexada por `InstanceID` e devolve `{Slot, Generation}`. O
slot está na mão no exato ponto da leitura. E `RaytracingHandle()` é o mesmo `GpuHandle` filtrado por
visibilidade, então `identity[i].Generation == 0` ⟺ a entrada era identidade — a guarda de hoje
transfere sem mudar de significado.

| slice | conteúdo | risco |
|---|---|---|
| `d28cb52` ✔ | contrato compartilhado, inversa em HLSL, paridade em WARP | nenhum — não muda comportamento |
| 4b | migrar `DualMotion` | baixo: já amarra identidade e remap |
| 4c | migrar `ReflectionMotion` (trace + mirror) | **médio** — mexe em descriptor table de passe RT |
| 4d | migrar `ReSTIRDISpatial` | **médio** — idem, e é o passe com 3 lookups por pixel |
| 4e | apagar buffer, SRVs e o laço da CPU; asserção de memória | baixo, depois dos três |

Cada uma de 4b–4d **mexe em reprojeção temporal**, que é onde uma troca de ordem aparece como
ghosting/shimmer e o suspeito natural é o denoiser. Vão atrás de A/B em GPU, uma por vez.

Decisão registrada: a inversa em HLSL é a **geral**, transcrita linha a linha do `Mat44::Inverse`, e
não a forma afim barata. As duas coincidem para TRS, mas a afim depende de uma garantia que nada
verifica. Especializar é um passo de **medição**, com a paridade em WARP como rede — e o custo atual
não é o que a fase persegue: são ~200 ALU sem tráfego nenhum.

## Riscos anotados

- A caixa de correio tem **um** consumidor documentado (`FGpuSceneInstanceTable::PrepareFrame`) e se
  esvazia na leitura. Um segundo leitor roubaria os avisos do primeiro em silêncio. Nada impede isso
  em tempo de compilação; a defesa é o comentário no header e este parágrafo.
- O teto da caixa é 4.096. Acima dele, varredura. Numa cena de 1M com muita coisa em movimento, o
  regime volta a ser o de antes — e é a escolha certa, porque uma lista maior que a cena não ajuda;
  o que resolveria de verdade é o produtor por célula, que é Fase 8.
- `IsUsable()` é **contrato, não mecanismo**: nada impede um consumidor futuro de expandir a lista
  sem consultá-lo. Como a lista vem vazia no overflow, ignorá-lo produz zero draws — falha alta, que
  é o desfecho preferível — mas continua sendo uma falha. O gate de verdade é o dimensionamento pelo
  `Stats().Candidates`, e ele só passa a ser exercitado quando a Fase 5 ligar o consumidor.
- O canário de overflow do shader **não é lido por ninguém** hoje. Ler contador de GPU para decidir
  draw esbarraria na regra 7 do plano (nenhum readback novo decide visibilidade), então o consumo
  correto dele é telemetria/asserção de debug, não política de frame. Fica anotado como decisão a
  tomar na Fase 5.
