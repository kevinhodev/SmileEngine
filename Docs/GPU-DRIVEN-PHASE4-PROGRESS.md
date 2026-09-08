# Fase 4 — estado da implementação

Data: 2026-09-08

Estado: **aberta, quatro slices entregues e uma rodada de revisão fechada.** Release fecha 64/64,
Debug compila e o cliente TypeScript compila. Dois dos seis itens do gate de saída estão fechados;
os outros quatro continuam abertos, e dois deles (`ObjectCB` e `TransformBuffer` do TemporalMotion)
ainda não foram tocados.

A revisão do Codex reprovou a primeira versão com seis achados, três bloqueadores — entre eles uma
regressão de motion vector que eu tinha introduzido e um trio de checagens que eu havia
**documentado como decisão deliberada**. Os seis estão corrigidos; a seção da revisão registra cada
um e o que o padrão deles ensina.

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

## O gate de saída, item a item

| # | Gate | Estado |
|---|---|---|
| 1 | cena estática envia zero dirty pages depois do assentamento | **fechado** — já valia pela saída antecipada por versão; agora está sob teste (`Smile.GpuSceneIncremental`) |
| 2 | mover um objeto não faz upload proporcional ao total | **fechado** — e o custo de *descobrir* também deixou de ser |
| 3 | remover/reusar slot respeita generation e fences | herdado da Fase 1; o teste novo cobre a linha zerada na remoção |
| 4 | 1M instâncias não exigem reconstrução/upload integral por frame | **provado em 4.096, não medido em 1M.** Falta a medição na sintética |
| 5 | `ObjectCB` e `TransformBuffer` do TemporalMotion deixaram de ser alocados | **não iniciado** |
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

### O padrão dos três primeiros

Os três P1 tinham a mesma forma: um comentário meu explicando por que o caminho mais fraco bastava.
Nos três casos o raciocínio estava certo **sobre o código de hoje** e errado sobre o que uma
checagem serve para fazer. Vale como regra para as fases seguintes: quando o comentário explica por
que **não** verificar, o comentário é o defeito.

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

Um achado do processo: a equivalência "range único vs sintéticos" **não** pega o range lido como
intervalo de slots — os dois modos cobrem o mesmo span, então a interpretação errada produz a mesma
saída nos dois. Quem pega é a comparação com os slots esperados, e por isso o teste ganhou uma
guarda explícita de que a lista de candidatos **não** é a identidade.

## Protocolo do MCP

Foi a **9**. `cpu.instanceTable.rowsScanned` entra no RESUMO, pela mesma regra do 5: é o número que
julga o gate, e um editor anterior publicaria `null` exatamente onde a medida decide.

## O que falta

1. **Medir na sintética de 1M.** A propriedade está provada em 4.096 objetos, em WARP. O gate fala
   de 1M, e o número tem de sair de uma rodada com o editor aberto. **Exige GPU.**
2. **Ligar a lista de candidatos ao frame** e publicar os contadores dela. Hoje `FGpuSceneCandidates`
   não tem chamador; o consumidor natural chega com a Fase 5.
3. **Aposentar o `ObjectCB`** — 256 B por item aceito, por frame, sempre. Some quando o VS passar a
   ler `Transform`/`PrevTransform` da tabela e a compor MVP com as matrizes do `FrameCB`. Precisa de
   `InstanceSlot` por draw (constante de root, como o `MaterialRow` da Fase 3) e mexe em oito call
   sites de submissão. **Mexe na imagem: vai atrás de A/B, como a Fase 3.**
4. **Aposentar o `TransformBuffer` do TemporalMotion** — 128 B por instância, por frame, sempre.
   Ele guarda `Current⁻¹·Previous` e `Previous⁻¹·Current`, derivados das duas colunas que já
   existem. A tensão real: a tabela derivada existe para **evitar inversão por pixel**, e os três
   consumidores (`DualMotion`, `ReflectionMotion`, `ReSTIRDISpatial`) indexam por `InstanceID` da
   TLAS, não por slot. Migrar exige passar pela tabela de identidade + remap e decidir onde a
   inversão passa a acontecer. Não é uma troca de ponteiro.
5. **Verificar a memória por medição**, não por leitura de código, como o gate exige.

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
