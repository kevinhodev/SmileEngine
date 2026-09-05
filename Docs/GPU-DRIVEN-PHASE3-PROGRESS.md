# Fase 3 — estado da implementação

Data: 2026-09-05

Estado: **implementada, revisada e estruturalmente completa; NÃO validada em GPU.** As slices estão
na árvore, Release fecha 61/61, Release e Debug compilam e o cliente TypeScript compila. O que falta
é o que exige placa: ligar o toggle na Bistro e na Emerald, provar paridade visual e medir. Enquanto
isso não acontece, a fase **não fecha**.

Base: Fase 2 fechada em `7382f21`. Nada da Fase 2 foi reaberto.

O default **não** mudou: `UseBindlessMaterials = false` (regra 3 do plano).

## O que foi entregue

| commit | slice |
|---|---|
| `d0c29ff` | capability e contrato de heap diretamente indexado |
| `d170f47` | descritores de material aposentados atrás das fences |
| `3ce6316` | bloco de raster na linha canônica de material |
| `c7fc69d` | G-buffer e z-prepass mascarado lendo material bindless |
| `c6e926a` | o A/B exposto no editor e a telemetria dele |
| `62a96fc` | memoização da consulta de linha nos passes migrados |
| `95721c8` | os quatro findings da revisão |
| `31c0136` | fecha as transações de textura e corrige a contagem por item |
| `926761c` | migra materiais masked das sombras e corrige o estado de leitura das tabelas |

## Findings da revisão (2026-09-05)

Revisão do proprietário, sem subagente: 1 P1 e 3 P2. Os quatro estão corrigidos, e os quatro
eram defeitos de verdade — nenhum é cosmético.

### P1 — a troca de textura não era atômica

`ApplySlotTexture` trocava ponteiro e flag **antes** de publicar, e a publicação chamava
`Allocate`, que **lança** quando o heap acaba. A exceção pulava `UpdateConstants`, as dirty
flags e o commit do OMM, deixando o material CPU descrevendo uma textura que a tabela GPU e o
CBV não conhecem — sem erro visível, até o próximo load — além de subir por um callback do Qt.

O conserto separa as duas alocações por quem chama: `TryAllocate` **recusa** e serve o caminho
de edição, que tem estado CPU para desfazer; `Allocate` continua lançando e serve o de load,
onde ficar sem descritor é fatal e não há o que desfazer. `RepublishTextureTable` virou
transacional — devolve `false` sem tocar em nada — e o bridge restaura ponteiro e flag na
recusa.

A revisão seguinte fechou a metade anterior da mesma transação: a textura importada ainda
reservava o seu descritor por `Allocate` **depois** de abrir e gravar a fila COPY. Heap cheio
continuava podendo lançar pelo callback Qt antes de alcançar a republicação segura. A importação
interativa agora reserva por `TryAllocate` antes de `Begin`; se a tabela do material recusar depois,
o bridge não persiste o override e descarta textura, procedência OMM e descritor recém-criados.
O caminho de load continua fatal.

### P2 — o reset republicava a tabela até oito vezes

Uma publicação por textura válida: oito tabelas alocadas, sete aposentadas que ninguém lê, e as
intermediárias descrevendo um material meio revertido. Perto do teto, a falha cairia no meio.
Agora os oito ponteiros são aplicados primeiro e há **uma** publicação — e as constantes só são
revertidas depois dela, senão uma recusa deixaria os fatores novos com as texturas antigas.

### P2 — o reload não coletava descritores já liberáveis

Depois de `Flush` + `WaitIdle`, `Required` cobre todos os ranges aposentados, mas o reload só
coletava `GpuSlots`. A cena nova começa a alocar antes do próximo `RenderFrame`, então podia
receber capacity exceeded com ranges liberáveis presos. Uma linha, no mesmo ponto e com o mesmo
`Required`.

### P2 — o contador de fallback incluía draws sem material

Este é o mais sério dos três P2, porque **o número que julga o A/B era o que estava errado**.

`Draws - BindlessDraws` supõe dois estados por draw e existem **três**: o z-prepass opaco
desenha a cena sólida sem tocar em material e nunca pode incrementar `BindlessDraws`, então
todo opaco aparecia como fallback. E não é propriedade do *passe*: nas sombras e na chuva só os
casters alpha-tested amarram material, no mesmo laço dos que não amarram.

`MaterialFallbackDraws` passou a ser contado por **item**, no ponto da submissão, e só quando
aquele item pediu material. A borda do MCP valida
`bindlessDraws + materialFallbackDraws <= draws` — sem exigir que **feche**, porque o terceiro
estado existe e é maioria em passes inteiros.

A guarda é simétrica: `Bindless` descreve o modo do passe, mas `BindlessDraws` só sobe quando o
item também pediu material. Isso é pré-requisito para migrar sombras opacas — elas podem viver
numa root bindless sem que cada caster leia uma linha de material.

## Continuação — sombras e estado das tabelas

As sombras do sol e locais têm root signatures próprias. Ambas ganharam o mesmo par de caminhos:
o PS clássico `ps_6_0` e a permutação bindless `ps_6_6`, com a capability participando da
disponibilidade global do toggle. A lista continua misturando dois tipos de caster de propósito:

- opaco usa PS nulo e **não pede material**;
- alpha-tested publica `MaterialRow` e lê a linha canônica.

As linhas são resolvidas depois do sort, uma vez por grupo de material. Isso preserva o objetivo
do A/B: tirar bindings sem acrescentar uma busca de hash por caster. `Smile.SubmitStats` cobre a
forma mista nos dois modos e exige que os opacos não inflem nem fallback nem bindless.

A implementação expôs um P1 anterior a esta slice: `FGpuTableStorage` criava e devolvia suas
tabelas para `NON_PIXEL_SHADER_RESOURCE`, herdado da Fase 2. Desde a primeira migração do G-buffer,
o pixel shader já lia a tabela canônica; Release compila esse erro e uma imagem pode até parecer
correta, mas a debug layer acusa estado incompatível. O estado estável agora é
`PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE`, preservando raster, RT e compute, com a
combinação presa no teste de ABI/WARP.

## A primeira tarefa: o que a verificação achou

O plano manda confirmar capability e contrato **antes de qualquer desenho**. Os cinco pontos que
ele listava foram conferidos um a um na árvore. Dois estavam certos como escrito, dois eram
lacunas reais e um estava certo pelo motivo errado.

**Certo como escrito.** A root signature do raster serializa com `D3D_ROOT_SIGNATURE_VERSION_1` e
só carrega `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`; `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` aparecia uma
única vez, no caminho de compute, e também por `VERSION_1`. O precedente de que funciona já
existia. E a versão da root signature **não** é a questão: a flag é valor de
`D3D12_ROOT_SIGNATURE_FLAGS`, enum compartilhado por `_DESC` (1.0) e `_DESC1` (1.1). Confirmado
contra a spec de HLSL Dynamic Resources, que só condiciona a flag a SM 6.6 + Resource Binding
Tier 3. Não houve `SerializeVersionedRootSignature` e o compute não foi tocado.

**Lacuna 1 — `ResourceBindingTier` nunca foi consultado.** O `FD3D12Device::Initialize` lia
`Options.ResourceHeapTier` e ignorava o campo *irmão do mesmo struct*. Ou seja: os passes de
compute que já usam `ResourceDescriptorHeap` em produção rodavam por sorte de hardware, não por
capability conferida. Agora ele é consultado, logado no boot e exposto por
`SupportsDirectHeapIndexing()`, com default conservador Tier 1 se a query falhar.

**Lacuna 2 — a ordem heap/root signature era acidente.** A spec:

> "SetDescriptorHeaps must be called, passing the corresponding heaps, before a call to
> SetGraphicsRootSignature or SetComputeRootSignature that uses either
> CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED or SAMPLER_HEAP_DIRECTLY_INDEXED flags."

O raster satisfazia isso, e por construção: o heap é amarrado no `BeginSceneRecording` e
reamarrado depois de cada um dos dois cortes de segmento, e os nove `SetGraphicsRootSignature`
caem todos depois de um desses pontos. Mas nada impedia que o próximo corte de segmento — ou um
passe novo — invertesse a ordem, e o sintoma não seria erro de API: seria `ResourceDescriptorHeap`
lendo de um heap que a GPU não tem amarrado. Os nove sítios passaram por
`FPipelineState::BindGraphicsRoot`, que amarra os dois na ordem certa. Reamarrar o **mesmo**
ponteiro de heap é explicitamente permitido; a proibição da spec é trocar de ponteiro depois da
root signature e antes do draw.

**Certo pelo motivo errado — descriptor lifetime.** A Fase 2 já tinha anotado o P1: trocar a
textura de um material reescrevia o descritor **no lugar**, no heap shader-visible, e a única
defesa possível era drenar as duas filas antes da escrita. Isso está fechado (abaixo). O que a
verificação acrescentou é que **null descriptors não eram escritos**: `WriteSRV` saía cedo para
slot sem textura. Passava porque o range nunca se movia e o shader consulta `HasXMap` antes de
amostrar — e deixaria de passar no instante em que a rotação existisse.

## Descriptor lifetime: o que mudou

`FTextureSRVHeap` ganhou `FreeDeferred`/`CollectRetired`, com o mesmo contrato do
`FGpuSceneSlots`: o range aposentado guarda até onde **cada fila** precisa chegar, e só volta ao
free list quando as duas passam. A coleta roda no mesmo ponto do frame que a de slots de cena —
separá-las daria duas chances de uma parar de rodar.

`UsedSlots` e `RetiredSlots` **não se sobrepõem**: o aposentado sai do primeiro e entra no
segundo, porque "quanto do heap está indisponível" é a soma dos dois. Contar o aposentado como
usado esconderia a fila de espera exatamente quando ela é o problema.

`FMaterial::RepublishTextureTable` aloca um range novo, escreve as **oito** posições e devolve a
base antiga. `Renderer::UpdateMaterialTextureSlot` perdeu `Flush`/`WaitIdle`. A base nova suja a
linha canônica do material — o `TextureBase` faz parte dela — e o `MarkMaterialRTStateDirty` que o
editor já levanta nessa mesma edição a sobe no frame seguinte.

As oito posições passaram a ser **sempre** escritas, incluindo as sem textura, com descritor
nulo — leitura definida devolvendo zero. Sem isso a rotação deixaria slots não inicializados
dentro de um range recém-alocado, e o preview de material copia os oito do staging sem consultar
flag nenhuma.

O `Finalize` continua reusando o range e reescrevendo no lugar, e agora está dito **por que isso
é seguro lá**: os três chamadores (default, import de cena e proxy do terreno) rodam com as duas
filas drenadas.

## A linha de material: 64 → 112 B

Os offsets 0..60 **não se mexeram**, e é isso que torna a mudança invisível para o ray tracing:
nenhum shader de hit precisou de uma letra. O bloco novo começa exatamente onde o bloco lido pelo
RT termina.

Entrou o que algum shader de raster lê e os 64 B de cima não carregavam: `RasterEmissive`
(rgb = fator × força, w = `AOStrength`), `NormalStrength`, `HeightScale`, os quatro parâmetros de
parallax realmente lidos, `ParallaxRefineSteps`, `ShadingModel` e seis bits novos.

Três decisões:

1. **Mesma linha, não uma segunda tabela.** A decisão D6 põe constantes de material no
   `FGpuMaterialRecord`, e uma segunda tabela duplicaria dirty rows, `RowOf` e diff — duas chances
   de "o material mudou" ficar errado.
2. **Dois emissivos, de propósito.** O de cima leva o `RTEmissiveScale` e o novo não. A
   divergência existe justamente para material decorativo parar de lavar a cena no indireto sem
   apagar na tela; derivar um do outro exigiria dividir por uma escala que pode ser zero.
3. **`ShadingModel` é VALOR, não bit.** São quatro modelos e o G-buffer escreve os dois bits de
   alpha do RGB10A2. O `kMaterialFlagFoliage` responde outra pergunta e não reconstrói o modelo
   Subsurface.

Ficaram de fora `ParallaxSelfShadow` e `ParallaxShadowSteps`: existem no `MaterialConstants` e no
espelho do CBV, e **nenhum** dos oito shaders que incluem `MaterialCB.hlsli` os lê.

### O que a linha perdeu

A garantia — declarada quando ela tinha 64 B — de que uma linha inteira cabe numa cache line de
64 B. Nunca foi medida, nem antes nem agora; 112 é múltiplo de 16, então toda linha ainda começa
alinhada a 16, e a tabela inteira continua cabendo em 24 KB na Emerald. Está anotado no header,
com a saída caso apareça num perfil: alinhar o stride a 128 e pagar 16 B mortos por material, e
não dividir a linha em duas tabelas.

**Isto é uma decisão reversível que o revisor pode querer discutir.** Ela está aqui explícita em
vez de escondida no diff.

## O caminho bindless

**Shader.** `MaterialCB.hlsli` passou a ter dois ramos que expõem os **mesmos nomes**: um
`#define` por campo sobre a linha canônica, e globais **estáticos** para a linha e para as oito
texturas. Estáticos, e não locais do `main`, porque os helpers de raster (`ApplyNormalMap`,
`ParallaxOcclusionMapping`, `ParallaxFade`, `MaterialAlphaClip`) são funções livres que leem os
campos do material — passá-los por parâmetro mudaria a assinatura de todas elas e faria o A/B
comparar **dois corpos de código diferentes**, que é exatamente o que a fase quer evitar.

Os shaders mudam duas linhas: a chamada de `SMILE_MATERIAL_LOAD()` e o guard nas próprias
declarações de textura.

Os bits rendem `uint` 0/1 e não `bool`, para paridade exata de tipo com o cbuffer — o `GBuffer.ps`
escreve `if (NormalFlipY == 0)`, e comparar bool com inteiro funciona por um caminho diferente do
original. O emissivo é o único que não é alias (na linha ele já chega multiplicado) e por isso
ganhou macro **nomeado**, em vez de um alias que mentiria sobre o conteúdo.

O padrão de global estático de recurso foi **verificado no DXC de produção antes de escrever
qualquer código de engine**, com uma sonda mínima: `static Texture2D` atribuído de
`ResourceDescriptorHeap` e lido de função livre compila em `ps_6_6`.

**Root signature.** Ganhou a flag, condicional à capability porque serializar o que o device
recusa derruba o boot, e duas constantes de **root** em b4: `[0]` descritor da tabela, escrito uma
vez por passe; `[1]` linha do material, o único estado por draw que sobra no caminho migrado —
4 B contra os 2 DWORDs de um root CBV mais a troca de tabela de descritores.

O parâmetro existe nos **dois** caminhos: omiti-lo sem Tier 3 daria duas root signatures com
índices diferentes, e o índice aparece em `FMaterial::Bind` e em todos os passes.

**Índice de textura seguro.** O `MaterialTextureIndex` devolve a própria base inválida de
propósito, para que um bit de flag errado apareça como descritor absurdo. No raster o índice é
usado para **criar** o handle, incondicionalmente, antes de qualquer `if (HasXMap)` — e
`ResourceDescriptorHeap[0xFFFFFFFF]` trava a GPU. Por isso o `Smile_RasterTextureSlot` devolve 0
para base inválida: ler o descritor 0 não trava, e o valor nunca chega a ser amostrado porque sem
tabela nenhum bit de mapa liga. Mesmo raciocínio já escrito no `FGpuMeshTable::GeometrySlotOf`.

## Um bug encontrado no caminho, e ele não era teórico

O `BuildDrawLists` troca material nulo ou não finalizado pelo `DefaultMaterial` (`ActiveMaterial`),
que **não estava na tabela** — `RowOf` o mandaria para a linha 0, que é "renderável sem material":
cinza 0,7 e sem tabela de textura. O caminho bindless desenharia esses objetos diferente do
clássico, que amarra o CBV e as texturas dele.

Ele passou a ter linha própria. E `MaterialRowMisses` conta, no ponto da submissão, todo item cujo
material caia na linha 0 **tendo material** — é o único jeito de esse erro aparecer como número em
vez de como objeto cinza numa captura.

## O A/B

Mesmo executável, mesmas listas de CPU, mesma ordem, mesmas draws. Só o caminho pelo qual o pixel
shader alcança o material muda. Toggle em Configurações → Renderização, card "GPU-driven".

`BindlessMaterialsReady()` exige Tier 3 **E** as quatro PSOs construídas; `SetBindlessMaterials`
**recusa** quando não dá, e o getter devolve o estado efetivo. Um knob que aceita e ignora é como
uma medição de A/B sai errada sem ninguém notar. A UI tem propriedade própria de
disponibilidade, então o toggle aparece desabilitado em vez de mentir.

### O que ficou no fallback, de propósito

| caminho | por quê |
|---|---|
| translúcidos (ForwardBlend) | decisão D9: transparência ordenada é fallback inicial |
| terreno | VB/IB e root próprios; já contava em `TerrainFallback` |
| chuva (occlusion map) | root do próprio subsistema |
| picking, contorno de seleção, preview de material | caminho de editor; o plano autoriza |

Nenhum deles é acidente. Os que ainda consomem material clássico aparecem em
`MaterialFallbackDraws`; caminhos sem material e subsistemas com telemetria própria não podem ser
reconstruídos como `Draws - BindlessDraws`, porque existe um terceiro estado por draw.

### O custo de CPU que o A/B não pode carregar

`FGpuMaterialTable::RowOf` é um `unordered_map`. Chamá-lo por item poria uma busca por hash
dentro do laço de submissão — custo de CPU **novo**, no caminho que a fase existe para
deixar mais barato, e ele apareceria na medição como se fosse do bindless. Os dois passes
memoizam por ponteiro de material, e as listas já vêm agrupadas por material.

O jeito certo é a instância **carregar** o `MaterialId` em vez de o passe consultar, e é o
que a Fase 4 faz quando o raster passa a ler a tabela de instâncias.

## Telemetria

Por passe: `bindlessDraws`, `materialFallbackDraws`, `materialRowSets` (par de `materialBinds` —
no A/B um zera e o outro assume o mesmo N) e `materialRowMisses`. Agregado: "Draws com material
fora do bindless" no painel.

Os dois primeiros são **exclusivos e não fecham com `draws`**, e isso é deliberado: draw que não
consome material — o z-prepass opaco inteiro — não é fallback de material. Ver o P2 acima.

Heap de descritores: `retired` (presos esperando as duas fences) e `overflows` (alocações
recusadas). O `Allocate` continua lançando, mas agora conta e **loga** antes: quanto foi pedido,
quanto está em uso, quanto está aposentado. "Acabou o heap" nunca dizia se a saída é subir o teto
ou caçar um consumidor que não devolve descritor.

Protocolo MCP em **8**, pela regra da v4: sobe quando o **cliente** passa a exigir o campo. A
borda passou a validar os contadores por passe — os três novos e os três antigos, que eram lidos
sem checagem — e o bloco `srvHeap` inteiro, mais duas coerências que uma checagem campo a campo
não pega: `bindlessDraws <= draws` e `used + retired <= capacity`.

## Provas

| o que | como |
|---|---|
| aposentadoria de descritor | `Smile.DescriptorLifetime` (WARP): fence parcial das duas filas, coleta fora de ordem, coalescência e o ciclo real de republicação |
| derivação do bloco de raster | `Smile.GpuMaterialRecord`: oráculo do `MaterialCB` transcrito à mão — os valores **efetivos** que o shader computava — em 128 casos exaustivos |
| ABI C++ ↔ HLSL | `Smile.GpuSceneTablesWarp`: os nove campos novos e as oito constantes novas, lidos pelo espelho de produção com o DXC do jogo |
| contrato do MCP | `Smile.EditorBridgeValidation`: campo a campo mais as coerências dos três estados |
| contagem de submissão | `Smile.SubmitStats` (WARP, command list gravada e nunca executada): draw sem material nos modos clássico **e bindless**, clássico, bindless, laço misto clássico/bindless e o pedido que não vaza por malha inválida |
| recusa com o heap cheio | `Smile.DescriptorLifetime`: enche o heap e exige base, ocupação e contador de falha intactos; a importação interativa recusa antes de abrir a fila COPY |

### Casos negativos rodados

| regressão injetada | resultado |
|---|---|
| coletar descritor olhando só a fila direta | 3 asserções vermelhas |
| devolver descritor direto ao free list | 9 asserções vermelhas |
| trocar `ParallaxMin`/`Max` de lugar no espelho HLSL | 8 asserções vermelhas em WARP |
| emissivo do raster levando o `RTEmissiveScale` | matriz inteira vermelha |
| tirar os três campos da Fase 3 da validação do MCP | 21 asserções vermelhas |
| remover a coerência `bindlessDraws <= draws` | 1 asserção vermelha |
| aceitar `undefined` no bloco `srvHeap` | 5 asserções vermelhas |
| voltar a `Allocate` que lança na republicação | o processo do teste TERMINA (exit 127) |
| voltar a `Allocate` na criação interativa da textura | o processo termina com `0xC0000409`, antes das asserções |
| voltar à conta antiga do fallback (`Draws - BindlessDraws`) | 3 asserções vermelhas |
| contar todo draw de um passe bindless | 5 falsos `BindlessDraws`, pegos pelo caso sem material |
| exigir que bindless + fallback FECHE com draws | recusa o passe sem material |

### Build e suíte

- **Release:** 61/61;
- **MCP/TypeScript:** `npm run build` limpo;
- **shaders:** as quatro permutações bindless compilam em `ps_6_6` sem warning.

## Falta na fase

1. **Validação em GPU.** Ligar o toggle na Bistro e na Emerald e comparar A/B no mesmo build e
   bookmark, com `CAPTURE-PROTOCOL.md`. O gate pede paridade em opaco, masked, two-sided,
   emissivo, motion vectors e guides de TAA/DLSS. **Nada disso foi feito** — não há placa neste
   ciclo.
2. **`materialRowMisses` observado em zero** numa cena real. O contador existe; ninguém o leu
   ainda com o caminho ligado.
3. **Sessão em Debug com a debug layer** trocando textura de material, para provar que o
   aposentamento fechou o `#921` sem reintroduzir dreno. A suíte sem placa não alcança hazard de
   lifetime de GPU — foi assim que a Fase 2 validou o conserto do OMM.
4. **Medir.** O plano proíbe declarar ganho sem baseline limpa. Nenhum número de perf foi
   produzido aqui, e não se deve supor que menos binds por draw signifique menos tempo de CPU sem
   medir: o `FDrawSubmitCache` já filtrava bind repetido, e a lista do G-buffer já vem agrupada
   por material.

## Riscos anotados, ainda não medidos

- **A linha de material dobrou de tamanho** (64 → 112 B). O caminho de hit do RT lê só a primeira
  metade, mas as linhas deixaram de começar em múltiplos de 64. Ver a nota acima.
- **O pixel shader do G-buffer ganhou uma leitura dependente** que não tinha: a linha do material
  vem de um `StructuredBuffer` em VRAM em vez de um CBV. Nas cenas atuais a tabela inteira cabe em
  24 KB e tende a ficar residente; a hipótese é que não seja perda, e medi-la exige GPU.
- **Oito handles de textura são criados por pixel**, inclusive os de mapas que aquele material não
  tem. São criações de handle, não amostragens, e o DXC elimina as que nenhum caminho usa — mas
  isso é uma expectativa sobre o otimizador, não uma medição.
