# Mesh lights em escala: plano para ReSTIR DI, ReGIR e meia resolução

> **Estado em 2026-08-17 — FASES 0 e 0.5 ENCERRADAS.** Da Fase 1 em diante, nada deve ser lido como
> implementado. Todo o detalhe está no §2; este bloco é só o resumo executável.
>
> ## O resultado
>
> `Amostragem + temporal` na Emerald (bookmark 0, TOD 22:00, M=8, 1573×804, RTX 3060 Ti):
> **31,09 → 2,58 ms, 12,05×.** Para o **ReSTIR DI completo**, **32,3 → 3,93 ms, ~8,2×** — o Pass B
> não lê a alias nem percorre o domínio, então não encolheu junto. **Nada disso tocou no estimador.**
>
> Duas causas, medidas em separado e por A/B com ordem alternada, assentamento e piso de ruído:
>
> 1. **§2.7 — a alias table morava num UPLOAD heap**, e era lida uma vez por candidata: ~10 milhões
>    de leituras aleatórias de 16 B por frame, atendidas em system memory pelo caminho PCIe. −87,9%.
>    O experimento isola RESIDÊNCIA; não separa cache, latência e banda.
> 2. **§2.11 — 85,5% do pool tinha fluxo zero** (228.374 → 33.160 com suporte positivo). −22,4% em
>    M=8 e −2,2% em M=1: o ganho escala com candidatas, porque cada candidata é um acesso ao domínio.
>
> ## Defaults resultantes
>
> | | estado | onde |
> |---|---|---|
> | alias em DEFAULT heap (VRAM) | **definitivo**, sem toggle | §2.7 |
> | suporte positivo compactado | **definitivo** | §2.11 |
> | `diInitialVisibility` | **ligado** | §2.13 |
> | RIS da Fase 1 | **adiado para a Fase 4** | §2.12 |
>
> ## Onde o custo está hoje (M=8 compactado, 2,58 ms de Pass A)
>
> | | ms | |
> |---|---|---|
> | raio de visibilidade inicial | ~1,66 | fixo (razão 0,98 entre M=8 e M=1), e **necessário** |
> | reuso temporal, reservoir, G-buffer | ~0,26 | fixo, não atribuído em detalhe |
> | candidatas (8 × 0,083) | ~0,66 | é o que o RIS atacaria |
>
> **Não sobrou corte fácil.** Desligar a visibilidade inicial devolve só 0,84 ms no DI — o Pass B
> cresce — e deixa o braço convergindo pior. E o RIS ganharia no máximo 0,66 ms contra um presample
> que o próprio gate da Fase 1 orçava em até 1 ms: como otimização do DI ele não se paga, e volta na
> Fase 4 como **infraestrutura do ReGIR**, com outro critério de aceite.
>
> ## Protocolo — o que esta série ensinou
>
> - **captura sacrificial** no início da sessão, e **assentamento** depois de todo toggle que dispare
>   reconstrução (a publicação do domínio limpa histórico e o protocolo aborta a captura — §2.8/§2.10);
> - **repetição do mesmo braço** como piso de ruído, e **célula de referência** repetida para medir a
>   deriva da sessão. Uma série foi descartada por spread interno de 73% (§2.11);
> - contraste A/B em **janela curta**; palíndromo sobre muitas células balanceia a média mas espalha
>   cada contraste demais;
> - o controle do mesmo braço mede o determinismo do instrumento, **não** o espalhamento entre dois
>   estimadores equivalentes. Para separar ruído de viés, varie a acumulação (§2.10).
>
> ## Limitações declaradas
>
> - **Não há medida de estabilidade temporal.** O protocolo produz imagem parada; caracterizar
>   cintilação e ghosting exigiria sequência com câmera em movimento. Registrado como limitação —
>   **decidido não estender o MCP agora**.
> - As comparações de imagem são de **luminância pós-tonemap**, nunca de energia física.
> - A Emerald tem **um** ponto fixo: os slots 0 e 1 dos bookmarks são a mesma pose.
> - A duplicata de 7,3 MB em VRAM (dois buffers DEFAULT de triângulo) fica, por decisão: é o preço de
>   o descritor amostrado nunca mudar, e não justifica drains nem complexidade. Orçamento completo no
>   §2.10 — **18.350.080 B de VRAM com alinhamento** e 18.269.920 B de payload em system memory.
> - O manifesto separa **payload** (`*PayloadBytes`, largura lógica) de **VRAM**
>   (`meshLightVramBytes`). Este último é a soma dos footprints alinhados em 64 KiB **atribuíveis aos
>   recursos** — **não** o total de heaps comprometidos pelo D3D12MA, que é de outra ordem e não se
>   obtém somando recursos. Para VRAM comprometida, o `VramTracker` por categoria.
> - `diMeshCandidates: 0` **zera a contribuição de mesh light**, e não "mantém o pool com reuso
>   temporal": a troca de valor limpa o histórico e nenhuma candidata nova aparece. Continua
>   permitido como ponto de diagnóstico — com o pool publicado os passes não caem no early-out —, e
>   se distingue do pool desligado no nome do arquivo (`M0` contra `Moff`).
> - O gate formal da Fase 0 tem os três critérios atendidos na Bistro (§2.3), e na Emerald o critério
>   (1) foi atendido a partir de `681c1f2`, com build limpa e bookmark declarado.
>
> A captura é automatizada pelo MCP local: `smile_capture_frame` restaura bookmark, aquece frames
> renderizados, fixa TOD e devolve PNG + manifesto; `smile_profile_configure` fixa o regime e os knobs.

## 1. Por que manter mesh lights

Mesh lights fazem a própria geometria emissiva iluminar a cena. Preservam a forma, a área, a cor e
o mapa emissivo do objeto, sem exigir uma point/spot light aproximada colocada pelo artista. Isso é
especialmente importante para janelas, letreiros, luminárias lineares e painéis grandes. Também
evita que uma malha brilhe para a câmera mas não participe da iluminação direta RT.

O custo não vem de “ter 228 mil luzes” em um loop linear. A alias table atual já escolhe um
triângulo em O(1). O custo observado vem de executar muitas avaliações por pixel, com leituras
globais divergentes no pool grande, reconstrução geométrica do triângulo e conversão da PDF de
área para ângulo sólido.

Portanto:

- mesh lights permanecem na engine;
- a alias table por potência permanece como distribuição global de referência;
- luzes analíticas e mesh lights continuam com orçamentos de propostas separados;
- o caminho antigo permanece disponível como A/B até o novo sampler fechar os gates;
- janela tratada como `Blend`/vidro no cooker é um problema de conteúdo e material, não será
  “corrigida” alterando energia ou visibilidade das mesh lights.

## 2. Evidência que motivou o plano

### 2.1 Cenas medidas

- Bistro exterior: **26.498 triângulos emissivos**, fluxo total aproximado **73,971**.
- Emerald Square: **228.374 triângulos emissivos**, fluxo total aproximado **17,563**.
- Na Emerald, o ReSTIR DI chegou a **9,70 ms**, dos quais **9,38 ms** estavam em
  `Amostragem + temporal` e só **0,32 ms** em `Espacial + visibilidade`.
- Em resolução nativa, o ReSTIR DI chegou a **31,41 ms**, com **30,22 ms** no primeiro passe e
  **1,19 ms** no segundo.
- A quantidade de pixels cresceu aproximadamente 2,98 vezes e o custo do DI 3,24 vezes. Isso é
  compatível com um passe full-screen quase linear em pixels; não aponta para um loop acidental
  por todos os triângulos.

Esses números vieram das capturas visuais da sessão e ainda não são uma série científica sob hash
limpo. Eles são suficientes para escolher a próxima investigação, mas a Fase 0 deve registrá-los
pelo protocolo de captura antes de fixar metas absolutas. **Foram reproduzidos — ver §2.3.**

### 2.2 O caminho atual

O código já possui:

- levantamento das malhas emissivas e extração GPU apenas quando a cena fica suja;
- `FTriangleLightGPU` de 32 bytes com base em fp32, arestas relativas em fp16, radiância RGB9E5 e
  fluxo por triângulo;
- readback diferido do fluxo e alias table de Vose construída na CPU;
- amostragem por potência em O(1), retornando a probabilidade exata do triângulo;
- amostragem uniforme na área do triângulo e conversão correta para PDF em ângulo sólido;
- reservoirs temporais e espaciais capazes de guardar índices do pool combinado;
- orçamento separado: atualmente até oito candidatas analíticas e oito candidatas de mesh lights
  por pixel quando os dois pools existem.

O que ainda não existe:

- presample RIS entre a distribuição global e o Pass A;
- acesso coerente por tile ou wave ao pool de triângulos;
- cópia compacta opcional do dado da luz junto do presample;
- mesh lights no `PT_AddDirectLocal`, no ReGIR ou nos seus consumidores secundários;
- caminho de meia resolução para o ReSTIR DI;
- captura efetiva do ReGIR com luzes não nulas;
- testes automatizados da distribuição, da PDF e da estabilidade dos índices de mesh lights.

### 2.3 Fase 0 medida (2026-08-17) — substitui o §2.1

RTX 3060 Ti, Emerald Square recozinhada na v8 do formato (estava em v7 — faltava o payload de RT
por triângulo), preset `controlled_native`, render 1573×804, `smile_profile_gpu` com 30 amostras.

⚠️ **Ressalva de protocolo:** a pose é a câmera default do editor (`bookmarkSlot: -1`), reprodutível
para um boot limpo mas **não é um bookmark declarado**. Para a série científica das fases seguintes,
fixar bookmarks — este é o único ponto do protocolo de captura que esta rodada não honrou.

Baseline com 8 candidatas de mesh:

| Passe | Mediana | % do DI |
|---|---|---|
| Frame (GPU) | 45,451 ms | — |
| **ReSTIR DI** | **31,749 ms** | **70% do frame** |
| Amostragem + temporal | 30,548 ms | 96,2% |
| Espacial + visibilidade | 1,201 ms | 3,8% |
| MeshLights (extract) | 0,000 ms | — |

O §2.1 registrava 31,41 / 30,22 / 1,19 ms. A reprodução bate dentro de ~1% nos três — **o custo alto
é real e reprodutível**, não artefato de medição.

Sweep do orçamento de mesh, tudo o mais igual:

| Candidatas de mesh | Amostragem + temporal | DI total | Marginal por candidata |
|---|---|---|---|
| 8 | 30,548 ms | 31,749 ms | — |
| 4 | 16,364 ms | 17,684 ms | 3,55 ms |
| 2 | 8,791 ms | 10,121 ms | 3,79 ms |
| 1 | 4,741 ms | 6,262 ms | 4,05 ms |
| pool desligado | 0,075 ms | **0,149 ms** | 4,67 ms |

Leitura, e é forte:

- o custo é **linear nas candidatas de mesh**, ~3,5 a 4,7 ms cada;
- com o pool desligado o DI inteiro cai para **0,149 ms**. Ou seja, **99,5% do custo do ReSTIR DI na
  Emerald é candidata de mesh light** — a Emerald não tem luz analítica (`giPunctualLightCount: 0`),
  então o pool analítico não contribui com nada;
- a extração custa **zero** (só roda quando sujo), confirmando que o gargalo não está nela;
- hipótese 1 do §3 CONFIRMADA (custo = pixels × candidatas), hipótese 3 CONFIRMADA (as 8 candidatas
  são orçamento de bring-up: 1 candidata já dá 6,26 ms contra 31,75).

**Gate de saída da Fase 0: NÃO fechado formalmente.** Dois dos três critérios estão satisfeitos —
(2) `Amostragem + temporal` em 96,2% do DI, acima dos 85% exigidos, e (3) o manifesto explica o que
rodou. O critério (1) **falha por protocolo, não por número**: o manifesto desta rodada registra
`"build": "fb99c7f-dirty"`, ou seja a instrumentação estava por commitar, e a pose não é bookmark
declarado. O custo foi reproduzido; o que falta é reproduzi-lo sob condições declaráveis.

Para fechar: commitar a instrumentação e repetir a baseline com bookmark declarado. Os números
acima bastam para ESCOLHER o próximo experimento, e não para fixar metas absolutas.

**Baseline limpa da Bistro (commit `8f59aea`, bookmarks 0 e 1, N=128, `controlled_native`):**

| Passe | slot 0 | slot 1 |
|---|---|---|
| Frame (GPU) | 15,154 ms | 15,041 ms |
| ReSTIR DI | 3,777 ms | 3,789 ms |
| Amostragem + temporal | 2,246 ms (59,5%) | 2,274 ms (60,0%) |
| Espacial + visibilidade | 1,524 ms | 1,510 ms |

Duas leituras que a Emerald sozinha escondia:

- o critério de 85% **não vale na Bistro** (59,5%). Ele foi escrito para "o caso crítico", e a
  medida confirma que o caso crítico é específico da Emerald — não é propriedade do ReSTIR DI;
- mesmos pixels e mesmas 8 candidatas, pool 8,6× maior (26.498 → 228.374), e
  `Amostragem + temporal` cresce **13,6×** (2,25 → 30,55 ms). Crescimento mais que proporcional ao
  domínio, que é assinatura de comportamento de MEMÓRIA e não de aritmética por candidata (a
  escolha é O(1) em qualquer N). ⚠️ Confound: a Emerald também tem TLAS bem maior, e o raio de
  visibilidade inicial mora dentro deste mesmo passe — o braço `diInitialVisibility: false` separa
  os dois, e deve entrar na Fase 0.5.

⚠️ **A Emerald ainda não tem bookmark declarado** (`Assets/Scenes/.../EmeraldSquare_Day.cameras.json`
não existe; só a Bistro tem). `CameraBookmarksBridge::Save` é `Q_INVOKABLE`, alcançável só pela QML —
não há caminho MCP. E o manifesto grava ângulos NÃO normalizados (`pitchDeg: -518.527`,
`yawDeg: 4512.04`) contra os normalizados que o sidecar guarda, então autorar o arquivo à mão a
partir dele não round-trip. O critério (1) da Emerald depende de gravar os bookmarks pelo editor.

### 2.4 ⚠️ 85,5% do pool da Emerald é peso morto

O contador de fluxo zero da Fase 0 mede, por cena:

| Cena | Extraídos | Fluxo zero | Degenerados | Contribuem de fato |
|---|---|---|---|---|
| Bistro | 26.498 | 5.795 (21,9%) | 0 | 20.703 |
| Emerald | 228.374 | **195.214 (85,5%)** | 0 | **33.160** |

São triângulos com área válida e radiância zero — conteúdo, não defeito de geometria (provavelmente
faces cujo mapa emissivo amostra preto). A alias table lhes dá probabilidade zero, então **não são
sorteados** e não corrompem a distribuição. Mas eles ocupam o pool inteiro:

- 6,25 MB dos 7,31 MB do buffer de triângulos;
- 3,12 MB dos 3,65 MB da alias table;
- o working set do sorteio é ~11 MB de acesso aleatório por candidata, contra **~1,6 MB** se o pool
  fosse compactado. A L2 da 3060 Ti (GA104, 256 bits) é de **4 MB**: ~11 MB não cabem, ~1,6 MB cabem.

**⚠️ CONFOUND que precisa ser separado antes de atribuir isto ao tamanho do pool.** A alias table
vive num **UPLOAD heap** (`MeshLights.cpp`, `CreateUploadBuffer`), isto é, em system memory: cada
candidata faz uma leitura aleatória de 16 B **sobre PCIe**, e são ~10 milhões delas por frame na
Emerald com 8 candidatas. Isso é uma hipótese de custo pelo menos tão forte quanto o tamanho do
domínio, e as duas são hoje indistinguíveis na medida. **Localização de memória e tamanho do
domínio têm de virar dois experimentos.**

**O que este achado NÃO autoriza:**

- **não** redimensionar nem cancelar o RIS. Um RIS de 131.072 entradas sobre 33.160 luzes não está
  "4× superdimensionado": as entradas são repetições PONDERADAS pela potência, e é justamente a
  repetição que cria a coerência por tile. `128 × 1.024` continua sendo a baseline do RTXDI, e sweep
  de tamanho só depois que o RIS existir e for medido;
- **não** concluir que a queda marginal por candidata (4,67 → 4,05 → 3,79 → 3,55 ms) prova cache.
  É compatível com cache e também com paralelismo de memória crescente; e o passe ainda carrega
  custo fixo, reuso temporal e o raio de visibilidade inicial. Só o A/B abaixo dá causalidade.

Compactar também não é um `if` no shader: o `MeshLightExtract` escreve em `OutLights[index]`, com
`index` sendo o índice global do triângulo. Compactar exige append com counter atômico ou —
preferível neste caminho estático — compactação na CPU depois do readback, com upload/cópia só do
prefixo ativo. Não precisa de passe novo, mas precisa de staging, contagem e tratamento explícito
dela.

### 2.5 Fase 0.5 — ordem acordada (2026-08-17)

A/B separado, **antes** da Fase 1 e sem realimentar o dimensionamento dela:

1. ✅ **FEITO** — instrumentação commitada (`8f59aea`, `52e8a27`, `67a2234`); baseline da Bistro
   repetida em build limpa com bookmarks 0 e 1 (§2.3), e o A/B do mesh pool registrado no §2.6.
   ⚠️ A Emerald continua sem bookmark: `CameraBookmarksBridge::Save` é `Q_INVOKABLE` e não há
   caminho MCP, então o critério (1) dela depende de gravar as poses pelo editor;
2. A/B da alias table: **UPLOAD contra DEFAULT heap**, com N fixo em 228.374 — isola localização.
   ⚠️ Correção de escopo: a Bistro valida CORREÇÃO, não decide desempenho — a alias dela tem
   423.968 bytes e cabe em qualquer cache. A decisão de desempenho espera a Emerald, cuja alias
   (3.653.984 bytes) fica na ordem do L2;
3. ✅ **FEITO (§2.10)** — A/B do domínio: pool original contra suporte positivo compactado
   (228.374 → 33.160). **−23,5%** em `Amostragem + temporal`, distribuição verificada neutra por
   discriminação de ruído contra viés. O caminho UPLOAD foi removido junto, já que a decisão fechou;
4. repetir M=8 e M=1 nos dois braços, e validar energia e distribuição, não só tempo;
5. ~~implementar o RIS index-only em `128 × 1.024`~~ — **re-justificado e NÃO recomendado como
   otimização do DI (§2.12)**: o teto de ganho caiu para 0,66 ms, menor que o presample de até 1 ms
   que o próprio gate da Fase 1 admite. Reavaliar na Fase 4, onde ele é infraestrutura do ReGIR;
6. ~~sweep do tamanho do RIS~~ — depende do item 5.

### 2.6 A/B do mesh pool na Bistro noturna — EXPLORATÓRIO (2026-08-17)

Commit `67a2234` limpo, bookmarks 0 e 1 declarados, TOD fixada em 22:00, N=128,
`controlled_native`, 1573×804. `giPunctualLightCount: 0` e 26.498 mesh lights amostráveis nos dois
braços. O ReSTIR DI **não** é alternado — só `meshLightsInPool`.

| | slot 0 ON → OFF | slot 1 ON → OFF |
|---|---|---|
| ReSTIR DI | 3,741 → 0,156 ms | 3,784 → 0,154 ms |
| Receptores, queda | **31,2%** | **46,8%** |
| Faixa [0,1), queda | 90,1% | 93,5% |
| Faixa [4,12), queda | 38,4% | 60,6% |

⚠️ **Leia como "queda da luminância PÓS-TONEMAP exibida nos receptores", e não como "fração da
luz".** A medida sai do composite em sRGB; média de código sRGB não é proporcional a radiância, e a
não-linearidade é maior justamente na faixa escura onde estão quase todos estes pixels. Nada aqui
autoriza uma afirmação sobre energia física.

⚠️ **O residual não é "emissão + GI".** É tudo que está fora da direta local: emissão própria no
G-buffer, lua, ambiente/IBL, reflexões, DDGI, ReSTIR GI e radiance cache — todos alcançam a
geometria emissiva pelo `ShadeSurfaceHit`, que não passa pelo pool do DI.

**Algoritmo exato da máscara** (para o número ser reproduzível e criticável):

- luminância por pixel = `0,2126·R + 0,7152·G + 0,0722·B` sobre o byte sRGB do PNG, sem linearizar;
- limiar de emissor `T = 40` (de 255);
- modo `união`: descarta o pixel se `lum(ON) > T` **ou** `lum(OFF) > T`;
- modo `só OFF`: descarta se `lum(OFF) > T`.

O modo `união` tem um viés conhecido: a máscara depende dos dois braços comparados, então um
receptor forte só no braço ON é descartado — exatamente onde a mesh light mais contribui —, o que
puxa a queda medida para BAIXO. O modo `só OFF` não depende da contribuição em teste, porque no
braço OFF a direta local vale zero por construção. Os dois foram medidos como teste de
sensibilidade e **concordam dentro de 0,6 pp** (31,20 contra 31,45; 46,77 contra 47,36), então o
viés existe e não muda a conclusão.

**Nenhum dos dois é o modo certo.** O certo é uma máscara vinda do EMISSIVO DO G-BUFFER: comum aos
dois braços e independente da diferença observada. Exige exportar o canal e não existe hoje.

**Gate do pool: sem bug, e provado sem o debug target.** O Pass B tem early-out explícito
(`ReSTIRDISpatial.cs.hlsl`): com `totalCount == 0` ele escreve `0.0` em `OutDirect`, `OutDiffuse`,
`OutSpecular` e `OutShadowMotion` e retorna; o Pass A grava reservoir inválido antes disso. Com o
pool desligado e `giPunctualLightCount: 0`, `totalCount` é exatamente zero, então a saída do DI é
preta por construção — e os 0,15 ms confirmam que os dois passes saem cedo. Decidido não estender o
bridge MCP para selecionar debug target: código, contagem zero e custo já fecham a dúvida.

### 2.7 🔴 A alias em UPLOAD heap era 88% do custo (2026-08-17)

Passo 2 da Fase 0.5, na Emerald. Commit `64b4bdc` limpo, bookmark 0, TOD 22:00, M=8, N=128,
`controlled_native`, initial visibility ligada, ordem **ABBABAAB**, primeira captura da sessão
descartada (ver §2.8). Só a heap lida varia; compactação, RIS, candidatas e visibilidade intocados.

| `Amostragem + temporal` | amostras | média |
|---|---|---|
| A — alias em **UPLOAD** (system memory, PCIe) | 30,994 / 31,003 / 31,021 / 31,342 | **31,090 ms** |
| B — alias em **DEFAULT** (VRAM) | 3,607 / 3,329 / 3,325 / 4,835 | **3,774 ms** |

**Δ = −27,32 ms, −87,9% — 8,2× mais rápido.** ReSTIR DI total: 32,3 → 5,0 ms (métrica secundária).

⚠️ **Dispersão do braço DEFAULT: 3,325 a 4,835 ms**, contra 30,994 a 31,342 do UPLOAD. O ganho não
está em dúvida — os intervalos nem se aproximam —, mas o 4,835 é um outlier no meio da série, e
portanto **descartar só a primeira captura não elimina todo outlier**. Ver §2.8.

Imagem idêntica, e o controle é o que sustenta isso: UPLOAD contra DEFAULT difere **exatamente o
mesmo** que UPLOAD contra UPLOAD do mesmo braço (0,02%, `|diff|` médio 0,0004, ~570 px de 1,26 M).
A troca é de residência, não de estimador — como esperado, já que os bytes são os mesmos.

**O que este experimento mede, e o que não mede.** Ele isola a **residência** da tabela: system
memory pelo caminho PCIe contra VRAM. Ele **não** separa cache, latência e largura de banda — os
três continuam agregados no número, e atribuir o ganho a qualquer um deles isoladamente seria ir
além do que foi medido.

**O que isto reescreve:**

1. Os números do §2.1 e do §2.3 — 9,70 ms e 31,41 ms — eram **majoritariamente custo de acessar
   system memory pelo caminho PCIe**, e não custo de amostragem. A alias é lida uma vez por
   candidata: 1,26 M pixels × 8 candidatas ≈ 10 milhões de leituras aleatórias de 16 B por frame.
2. A hipótese 2 do §3 ("o pool de 228 mil piora localidade") estava certa no efeito. Sobre o
   mecanismo, o que a medida autoriza é: **a residência era o fator dominante; o efeito do tamanho
   do domínio ainda precisa ser remedido** com a alias já em VRAM.
3. A superlinearidade do §2.3 — pool 8,6× maior, passe 13,6× maior — é **consistente com essa
   causa**. Não é prova: a comparação entre Bistro e Emerald carrega outros fatores (TLAS, conteúdo,
   profundidade de cena) que este experimento não controlou.
4. O §2.4 (85,5% de fluxo zero) precisa ser **remedido** com a alias em VRAM. O peso morto continua
   existindo, mas o argumento de "não cabe na L2" foi construído somando os dois buffers como se
   fossem a mesma moeda — e o de UPLOAD nem VRAM era.

**O que este resultado NÃO decide, e por quê:** ele não redimensiona nem cancela o RIS. O RIS
resolve coerência de acesso entre pixels vizinhos, que é um problema diferente de latência de
barramento, e pode continuar valendo sobre uma baseline de 3,77 ms. Mas a premissa que motivou o
dimensionamento mudou de ordem de grandeza, então a Fase 1 deve ser **re-justificada** contra a nova
baseline antes de ser implementada — não herdada.

✅ **DEFAULT virou o padrão** (`AliasDefaultRequested = true`). O A/B é grande demais, reprodutível
e semanticamente neutro; UPLOAD heap para uma estrutura de leitura aleatória por candidata é escolha
errada, não trade-off. O caminho UPLOAD **fica** como A/B temporário — é o único jeito de reproduzir
a baseline antiga — e sai quando não houver mais o que comparar.

### 2.8 Achado de protocolo: descartar a primeira captura da sessão

No A/B da Bistro, as capturas [2], [3] e [4] convergiam em luminância de receptores 0,4353-0,4354 e
só a [1] destoava (0,4397). Duas capturas quentes do mesmo braço diferem em **14 pixels de 1,26 M**,
então o instrumento é praticamente determinístico e o outlier é visível quando se procura.

Sem o controle do mesmo braço, essa diferença teria sido atribuída ao eixo em teste: ela tinha
exatamente a mesma magnitude que a diferença entre os braços.

E a captura sacrificial **não basta sozinha**: no §2.7, mesmo com ela, o braço DEFAULT produziu
3,325 / 3,329 / 3,607 / **4,835** ms — um outlier no meio da série, longe do boot. Descartar a
primeira remove o transiente de aquecimento, não a variabilidade do resto.

**Portanto o protocolo exige as DUAS coisas:**

1. **captura sacrificial** no início da sessão, descartada;
2. **repetição do mesmo braço** dentro da série, servindo de piso de ruído — sem ela não há como
   dizer se uma diferença observada é do eixo em teste ou da dispersão do instrumento.

Ordem alternada (ABBA, ABBABAAB) continua necessária por cima das duas, para a deriva térmica
atingir os dois braços de forma simétrica.

### 2.9 Sweep de candidatas com a alias em VRAM (2026-08-17)

Commit `681c1f2`, Emerald, bookmark 0, TOD 22:00, `controlled_native`, captura sacrificial
descartada, e **M=8 repetido no começo e no fim** como piso de ruído dentro da própria série.

Regressão do default: com `meshAliasDefaultHeap` **ausente** do payload, o readback devolve `true`
e o manifesto sai `requested: true, effective: true`, com etiqueta `-aliasDef`.

| M | `Amostragem + temporal` | ReSTIR DI | Frame (GPU) |
|---|---|---|---|
| 8 | 3,614 ms | 4,962 ms | 21,422 ms |
| 4 | 2,994 ms | 4,408 ms | 20,902 ms |
| 2 | 2,327 ms | 4,061 ms | 20,606 ms |
| 1 | 2,020 ms | 3,746 ms | 20,354 ms |
| 8 (repetição) | 3,600 ms | 4,997 ms | 21,283 ms |

**Piso de ruído da série: 0,013 ms** (as duas medidas de M=8). Toda diferença acima disso é sinal.

**Custo marginal por candidata**, contra a era UPLOAD do §2.3:

| Degrau | Agora (VRAM) | Antes (UPLOAD) | Razão |
|---|---|---|---|
| 8 → 4 | 0,155 ms | 3,55 ms | 23× |
| 4 → 2 | 0,334 ms | 3,79 ms | 11× |
| 2 → 1 | 0,307 ms | 4,05 ms | 13× |

**O achado desta série: a candidata deixou de dominar o passe.** Em M=1 o passe ainda custa
2,020 ms, e com o pool desligado ele custava 0,075 ms (§2.6, Bistro; o mecanismo é o early-out, que
vale igual aqui). Extrapolando o marginal, sobra da ordem de **1,7 ms que não é por candidata** —
raio de visibilidade inicial, reuso temporal e tráfego de reservoir — contra ~1,9 ms das oito
candidatas. Ou seja, em M=8 o custo está hoje **repartido em cerca de meio a meio**, quando antes as
candidatas eram ~94% do passe.

Isso importa diretamente para a Fase 1: **o RIS amortiza a metade "por candidata", não o passe
inteiro.** O teto de ganho dele encolheu junto com a baseline. Não é argumento para cancelá-lo — é o
número contra o qual a re-justificativa do §2.7 tem de ser feita.

⚠️ Comparação de `Frame (GPU)` com os 45,451 ms do §2.3 **não é válida**: aquela medida foi na câmera
default com TOD 10:00, esta é no bookmark 0 com TOD 22:00. Os passes do DI são comparáveis porque o
§2.7 usou exatamente esta pose e esta hora.

### 2.10 Compactação do suporte positivo: −23,5% (2026-08-17)

Passo 3 da Fase 0.5, na Emerald. Commit `b9499ac`, bookmark 0, TOD 22:00, M=8, N=128,
`controlled_native`, sacrificial descartada, ordem **ABBABAAB**. Sem RIS, sem mexer em initial
visibility, com a alias já em VRAM nos dois braços — só o **tamanho do domínio** varia.

| | domínio | alias, domínio em B | triângulos, domínio em B | `Amostragem + temporal` |
|---|---|---|---|---|
| A — sem compactar | 228.374 | 3.653.984 | 7.307.968 | 3,582 / 3,328 / 3,337 / 3,340 → **3,397 ms** |
| B — compactado | 33.160 | 530.560 | 1.061.120 | 2,579 / 2,598 / 2,609 / 2,608 → **2,598 ms** |

**Δ = −0,798 ms, −23,5%.**

⚠️ **"Domínio em bytes", e NÃO "bytes tocados".** Não há instrumento de tráfego aqui: o número sai
igual com o DI desligado, com zero candidatas ou com a tabela em construção. É o tamanho do conjunto
sobre o qual o sorteio *aconteceria* — útil porque é ele que se compara contra o tamanho do cache.

⚠️ **A ALOCAÇÃO não muda, e é maior do que parecia.** São SEIS buffers, e o orçamento real na
Emerald é:

| | payload, VRAM | payload, system memory |
|---|---|---|
| triângulos, saída da extração | 7.307.968 | — |
| triângulos, cópia amostrada | 7.307.968 | — |
| alias | 3.653.984 | 3.653.984 (staging) |
| staging dos triângulos | — | 7.307.968 |
| readback | — | 7.307.968 |
| **soma dos payloads** | 18.269.920 | 18.269.920 |
| **VRAM com alinhamento (64 KiB/recurso)** | **18.350.080 B** | — |

⚠️ **PAYLOAD e ALOCAÇÃO são coisas diferentes, e o manifesto traz as duas.** Recurso D3D12 tem
alinhamento de alocação — 64 KiB para buffer —, então somar larguras lógicas e chamar o resultado de
VRAM subestima. Na Emerald a diferença é de 80.160 B (0,44%), mas o mecanismo é por recurso: um pool
de **um** triângulo emissivo tem 32 B de payload e ocupa 64 KiB, um fator de 2048.

**Definição exata de `meshLightVramBytes`:** soma dos **footprints alinhados atribuíveis aos
recursos** que moram em VRAM. **Não** é o total de heaps comprometidos pelo D3D12MA — e a diferença
não é sutil:

- o alocador pede blocos muito maiores que estes recursos, então o heap comprometido é de outra
  ordem e **não se obtém somando recursos**;
- quando dois destes caem no mesmo bloco, esta conta **superestima** o custo marginal, porque cobra
  o alinhamento de 64 KiB uma vez por recurso e a realidade nem sempre cobra.

É uma **atribuição por recurso**, útil para orçamento comparativo entre fases do plano. Quem
responde "quanta VRAM a engine comprometeu" é o `VramTracker`, por categoria.

Os **dois** buffers DEFAULT de triângulo são o preço de o descritor amostrado nunca mudar: alternar
entre original e compacto exigiria recriar o SRV e drenar as filas, porque as tabelas de trace do DI
guardam cópia do descritor. É troca deliberada, e fica registrada porque um campo único de "bytes de
triângulo" reportava 7,3 MB quando a VRAM tinha 14,6.

**Neutralidade da distribuição: verificada, e não assumida.** Os triângulos descartados tinham
probabilidade zero, então a compactação deveria ser neutra. O teste de imagem inicial acusou o
contrário — e estava certo:

1. **Bug encontrado pela imagem.** O vetor de probabilidades do Vose era preenchido só com o suporte
   positivo, mas o domínio caía para `NumTriangles` quando a compactação estava desligada: o laço
   indexava além do fim do vetor, e o braço de **controle** construía ~195 mil entradas da alias a
   partir de memória arbitrária. O tempo sozinho não denunciaria — os dois braços tinham dispersão
   interna mínima e pareciam uma medida limpa. Corrigido em `b9499ac`.
2. **Depois do fix, a diferença de imagem persistia (1,58%), e o controle NÃO a limita.** O par
   A-contra-A dá 0,02% porque é o *mesmo* sampler com as mesmas sementes. Dois samplers
   equivalentes mas distintos — o mesmo `u0` mapeia para entradas diferentes — produzem realizações
   diferentes sem que haja viés.
3. **Discriminador: acumulação.** Ruído encolhe com N; viés persiste.

| N | A | B | diferença |
|---|---|---|---|
| 128 | 1,5779 | 1,5530 | +1,58% |
| 512 | 1,5452 | 1,5503 | **−0,33%** |

A diferença cai para um terço **e troca de sinal** — assinatura de ruído de Monte Carlo, não de
viés —, e em N=512 fica dentro do portão de 1% de energia do plano. Nota lateral: é o braço **A**
que ainda estava convergindo em N=128 (1,5779 → 1,5452, 2,1%), enquanto B praticamente não se moveu
(0,17%).

**Lição de método, que vale para todo A/B seguinte:** o controle do mesmo braço mede o determinismo
do instrumento, **não** o espalhamento entre dois estimadores equivalentes. Quando os dois braços
consomem a aleatoriedade de forma diferente, a única forma de separar ruído de viés é variar a
acumulação e ver para onde a diferença anda.

**⚠️ Trocar o knob dispara reconstrução, e a captura seguinte é sacrificial.** A publicação do
domínio novo limpa o histórico; caindo dentro do aquecimento, o protocolo aborta com *"um ajuste
derrubou historico durante o aquecimento"* — comportamento correto. Toda matriz que alterna este
knob precisa de uma captura de assentamento depois de cada troca, além da sacrificial de sessão.

Isso só ficou visível depois de a invalidação passar a acontecer na PUBLICAÇÃO: com o clear no
pedido, o domínio trocava no meio do aquecimento em silêncio e o guard nunca disparava.

Regressão de ida e volta (`a54cebf`, sem-compactar → compactado → sem-compactar → compactado):
`requested`/`effective` nunca divergem, a etiqueta acompanha, e a imagem repete — A1 contra A2 dá
`|diff|` médio 0,0001 (179 px de 1,26 M) e B1 contra B2 dá 0,0001 (87 px).

### 2.11 Matriz definitiva da Fase 0.5 (2026-08-17, `047044b`)

Emerald, bookmark 0, TOD 22:00, `controlled_native`, N=128. Sacrificial de sessão, **assentamento
após cada toggle**, e cada contraste A/B em bloco ABBA curto.

⚠️ **A primeira tentativa desta série foi descartada, e o motivo vale registrar.** Ela usava um
palíndromo sobre as 8 células: cada célula ganhava um slot cedo e um tarde, o que balanceia a média
mas espalha cada contraste por ~8 minutos. O spread interno das células de M=8 deu **73%**
(2,585 e 4,483 ms na mesma célula), inutilizando as médias. A v2 põe cada contraste numa janela
curta e adiciona uma **célula de referência** repetida no início, meio e fim — que mediu a deriva da
sessão em **1,0%** (3,302 / 3,335 / 3,331 ms) e portanto **descarta deriva térmica** como explicação
dos outliers da v1. A causa deles não foi identificada; o que mudou foi o desenho ficar imune a ela.

| | sem compactar | compactado | Δ |
|---|---|---|---|
| **M=8** | 3,320 / 3,328 → **3,324 ms** | 2,573 / 2,584 → **2,578 ms** | **−0,745 ms, −22,4%** |
| **M=1** | 2,048 / 2,039 → **2,043 ms** | 1,998 / 2,001 → **1,999 ms** | **−0,044 ms, −2,2%** |

Spread interno por célula: 0,003 a 0,010 ms — duas ordens abaixo dos efeitos medidos.

Convergência em N=512, os dois braços: M=8 dá **−0,33%** e M=1 dá **+0,24%** de energia. Dentro do
portão de 1%, com sinais opostos — ruído, não viés, nos dois orçamentos de candidatas.

**A leitura estrutural.** O ganho da compactação escala com o número de candidatas, porque cada
candidata é um acesso aleatório ao domínio: 22,4% em M=8 contra 2,2% em M=1. Isso decompõe o passe:

| | por candidata | fixo (extrapolado a 0 candidatas) |
|---|---|---|
| sem compactar | (3,324 − 2,043)/7 = **0,183 ms** | ~1,86 ms |
| compactado | (2,578 − 1,999)/7 = **0,083 ms** | ~1,92 ms |

Compactar **mais que dobra a eficiência por candidata**. E o resultado é que em M=8 compactado o
passe de 2,578 ms é ~1,92 ms de custo FIXO — visibilidade inicial, reuso temporal, tráfego de
reservoir — mais ~0,66 ms de candidatas. **As candidatas são hoje 26% do passe.**

### 2.12 Re-justificativa do RIS contra a nova baseline

O §2.7 exigiu que a Fase 1 fosse re-justificada em vez de herdada. Com o §2.11 o número existe.

Trajetória de `Amostragem + temporal` na Emerald, M=8, mesma pose e hora, **sem tocar no estimador**:

| | ms | o que as candidatas representavam |
|---|---|---|
| alias em UPLOAD (§2.1, §2.3) | 31,09 | ~94% do passe |
| alias em VRAM (§2.7) | 3,32 | — |
| \+ suporte compactado (§2.11) | **2,58** | **26% do passe** |

**12,05× — e isso é do PASS A.** Para o **ReSTIR DI completo** a comparação é **32,3 → 3,93 ms, cerca
de 8,2×**: o Pass B não mudou (ele não lê a alias nem percorre o domínio), então ele não encolheu
junto e passou a pesar mais na proporção. Citar 12× para o DI inteiro seria transportar o ganho de
um passe para o outro.

O RIS amortiza o sorteio POR CANDIDATA. Isso era ~28,7 ms do passe original; hoje são **0,66 ms**.
Um RIS perfeito, que tornasse o sorteio gratuito, ganharia no máximo **0,66 ms de 2,58** — e o gate
de desempenho da própria Fase 1 admite um presample de **até 1 ms** (§5). **O teto do ganho é menor
que o teto do custo que o plano já autorizava.** Como otimização do DI, o RIS não se paga.

**Isso NÃO é o mesmo que "o RIS é inútil", e a diferença importa:**

- a Fase 4 precisa de um pool pré-amostrado para alimentar o ReGIR — o §4 desenha o RIS como fonte
  comum, e ali ele é INFRAESTRUTURA, não otimização;
- o custo por candidata cresce com resolução e com orçamentos maiores; a conclusão acima vale para
  1573×804 com M=8, e não é uma lei;
- o RIS ataca coerência entre pixels vizinhos, que é um eixo que nenhum dos dois experimentos da
  Fase 0.5 tocou.

**Recomendação:** não implementar o RIS como otimização do ReSTIR DI. Reavaliá-lo quando a Fase 4
(mesh lights no ReGIR) for aberta, onde ele entra por outro motivo e com outro critério de aceite.
O alvo natural do próximo trabalho de desempenho no DI passa a ser o **custo fixo de ~1,92 ms**, que
é 74% do passe — e cujo maior componente declarado é o raio de visibilidade inicial, que já tem
toggle e nunca foi medido isoladamente.

### 2.13 O raio de visibilidade inicial é custo FIXO — e não é desperdício (2026-08-17)

Emerald, `047044b`, bookmark 0, TOD 22:00, suporte compactado, ABBA em janela curta, assentamento
após cada toggle. Deriva de sessão medida por célula de referência: 2,576 / 2,595 / 2,604 ms — 1,1%.

**O teste da parcela fixa, que era a pergunta:**

| | economia no Pass A ao desligar |
|---|---|
| M=8 | 1,651 ms |
| M=1 | 1,678 ms |
| **razão** | **0,98** |

Perto de 1, não de 8. **Confirmado: o raio de visibilidade inicial não escala com candidatas — é
parcela fixa.** É o único componente do custo fixo que agora está medido isoladamente.

**⚠️ Mas o ganho no DI é METADE disso, porque o Pass B cresce:**

| | Pass A | Pass B | DI total |
|---|---|---|---|
| M=8 visibilidade ligada | 2,591 | 1,349 | **3,932** |
| M=8 desligada | 0,940 | 2,154 | **3,095** |
| M=1 ligada | 2,006 | 1,430 | **3,440** |
| M=1 desligada | 0,327 | 2,452 | **2,780** |

Economia real: **0,837 ms** em M=8 (21,3% do DI) e 0,660 ms em M=1. Sem a rejeição inicial, a
candidata ocluída sobrevive à seleção e o custo migra para o resolve — não desaparece.

**Qualidade — e aqui o knob deixa de ser gratuito.** Luminância **pós-tonemap** dos receptores em
N=512 (não é energia física):

| | ligada | desligada | diferença |
|---|---|---|---|
| M=8 | 1,5491 | 1,5335 | **−1,00%** |
| M=1 | 1,6394 | 1,5514 | **−5,37%** |

⚠️ **Isto é diferença de CONVERGÊNCIA naquela realização, e não prova de escurecimento permanente
nem de viés.** Os dois braços são estimadores que devem convergir para a mesma resposta: a
visibilidade inicial rejeita a amostra ocluída que o resolve mataria de qualquer forma, então ela
muda variância e velocidade, não o alvo. E o §2.10 já mostrou, com o mesmo instrumento, que
diferenças desta magnitude trocam de sinal quando a acumulação cresce — o discriminador de ruído
contra viés **não foi rodado para este A/B**.

O que a medida sustenta: em N=512 o braço desligado está mais longe da resposta, o que é o esperado
de um estimador que gasta seleções em amostras ocluídas. Em M=8 o desvio fica na borda do portão de
1% do plano; em M=1 ele o estoura por cinco vezes. Caracterizar isso como estabilidade exigiria
sequência temporal, que este protocolo não produz.

**Ruído espacial** (média de |lum(x) − média dos 8 vizinhos| nos receptores, N=512):

| | ligada | desligada | idem, normalizado pela média |
|---|---|---|---|
| M=8 | 0,96739 | 0,95098 | 0,6245 → 0,6201 |
| M=1 | 1,11145 | 0,98707 | 0,6779 → 0,6362 |

⚠️ **Esta métrica não resolve a pergunta de estabilidade sozinha.** Contraste local absoluto cai
quando a imagem escurece, e o braço desligado É mais escuro; a normalização pela média reduz o
efeito mas não o elimina, porque a relação entre tonemap e contraste não é linear. O que se pode
afirmar é que **ela não mostra o braço desligado como mais ruidoso** — não que ele seja mais limpo.
Uma medida honesta de estabilidade pede sequência temporal com câmera em movimento, que este
protocolo de imagem parada não produz.

**Conclusão.** Desligar a visibilidade inicial não é otimização: troca 0,84 ms do DI (21%) por um
braço que converge pior — 1% de desvio em M=8 e 5,4% em M=1 no mesmo N. O raio está pagando o que
custa: é ele que mantém a amostra do reservoir útil, e sem ele o trabalho reaparece no Pass B **e** a
imagem fica mais longe da resposta no mesmo orçamento de acumulação. Ele fica como knob de A/B, e
**não** como candidato a corte.

Sobram ~1,92 − 1,66 ≈ **0,26 ms** de custo fixo ainda não atribuídos (reuso temporal, tráfego de
reservoir, decode de G-buffer). O custo fixo do passe está, portanto, explicado: ele é quase todo o
raio de visibilidade inicial, e ele é necessário.

## 3. Diagnóstico e hipóteses a provar

O plano parte das hipóteses abaixo. Elas não devem virar “fatos” sem a Fase 0.

1. O custo dominante é a multiplicação `pixels × candidatas de mesh light`, não a extração nem a
   construção esporádica da alias table.
2. O pool de 228 mil elementos piora localidade e variância, embora a escolha do índice seja O(1).
3. Oito candidatas por pixel são um orçamento herdado de bring-up, não um mínimo de qualidade já
   demonstrado.
4. Presample em um conjunto pequeno e renovado de tiles deve manter a PDF por potência e amortizar
   o sorteio global. Só a variante com dado compacto promete remover também o fetch aleatório do
   triângulo; o plano mede as duas para não atribuir ganho à etapa errada.
5. Duplicar o triângulo compacto no buffer RIS pode valer a VRAM adicional, mas só depois de medir
   a variante que guarda apenas `{ índice, 1/pdf }`.
6. A grade atual do ReGIR, esticada pela AABB inteira da cena, é grosseira demais para ser a solução
   final da Emerald. Alimentá-la diretamente com 228 mil triângulos antes do presample repetiria o
   problema em outro passe.

## 4. Arquitetura alvo

O fluxo alvo é:

```text
geometria/material emissivo
    -> extração estática ou quando sujo
    -> distribuição global por potência
    -> presample RIS renovado e organizado em tiles
       -> ReSTIR DI: propostas coerentes para superfícies primárias
       -> ReGIR: propostas globais para construir células de mundo
          -> DDGI / reflexos / ReSTIR GI / radiance-cache updater
```

A primeira implementação não precisa criar a textura PDF com mip chain usada pelo RTXDI. A alias
table de Vose da SmileEngine já representa a mesma distribuição discreta por potência e devolve a
probabilidade exata. Ela pode alimentar diretamente o presample RIS. A textura PDF só entra se uma
medida mostrar que o mip descent ou a atualização totalmente GPU traz vantagem concreta.

### 4.1 Formato inicial do RIS

- **128 tiles × 1.024 entradas**, igual ao default do RTXDI local: 131.072 amostras.
- Entrada mínima: `uint2 { LightIndex, asuint(InvSourcePdf) }`, total de **1 MiB**.
- Cada grupo 8×8 do Pass A escolhe um tile com RNG coerente; cada lane escolhe entradas diferentes
  dentro desse tile.
- O índice continua no domínio combinado existente, ou recebe tag explícita somente quando o
  ReGIR passar a misturar luz analítica e triângulo.
- O presample deve ser renovado a cada frame inicialmente. Congelá-lo pode excluir permanentemente
  luzes do conjunto finito e transformar ruído temporal em erro persistente. Atualização parcial ou
  menos frequente só entra após A/B.

### 4.2 Dado compacto opcional

Depois do RIS mínimo, medir uma segunda variante que armazena uma cópia de `FTriangleLightGPU` na
mesma ordem das entradas RIS. Ela acrescenta aproximadamente **4 MiB** aos 131.072 slots, mas pode
eliminar o fetch aleatório de 32 bytes no pool de 228 mil triângulos. O índice e a PDF continuam no
buffer mínimo para temporal, debug e fallback.

O dado compacto é uma otimização, não uma mudança de estimador. Deve ser commit e gate separados.

## 5. Ordem de execução

### Fase 0 — baseline e telemetria efetiva

Objetivo: provar onde o tempo está indo e criar um registro que sobreviva à sessão atual.

Implementar ou registrar:

- usar `smile_capture_frame` como caminho padrão da matriz: `bookmarkSlot`, `warmupFrames`, preset
  `scientific`/`gameplay`, TOD fixada e timeout explícito; a chamada permanece aberta até o editor
  publicar PNG + manifesto, portanto não é necessário observar a UI ou adivinhar a convergência;
- manifesto: `meshLightCount`, `meshLightTotalFlux`, `meshAliasReady`, candidatas analíticas e de
  triângulo, resolução interna do DI e toggles solicitado/efetivo para RIS, compact data e meia-res;
- escopos GPU separados para presample futuro e, se necessário, uma variante de diagnóstico que
  separe geração inicial de reuso temporal;
- contadores opcionais de candidatas válidas, PDF zero, triângulos degenerados, reservoir vazio,
  seleção analítica/mesh e amostra ocluída;
- memória por recurso: triângulos, alias, RIS e RIS compact;
- captura Emerald e Bistro em dois pontos fixos, resolução nativa e escala reduzida;
- sweep isolado de 8, 4, 2 e 1 candidatas mesh pelo caminho alias atual, sem RIS, para separar
  custo de avaliação por candidata de custo de seleção/memória;
- A/B com mesh lights no pool ligado/desligado e com visibilidade inicial ligada/desligada.

Os contadores GPU são instrumentação não neutra. Capturas de desempenho e imagem devem registrar
quando eles estão desligados; a série com estatísticas não pode ser misturada à série limpa.

Gate de saída:

- reproduzir o custo alto sob hash limpo;
- demonstrar que `Amostragem + temporal` continua sendo pelo menos 85% do DI no caso crítico;
- ter um manifesto que explique se mesh lights e cada otimização realmente rodaram.

### Fase 1 — presample RIS por potência

Objetivo: amortizar o sorteio na distribuição global e criar a abstração de propostas coerentes,
sem mudar a distribuição alvo. Esta fase mínima ainda busca o triângulo original por índice.

Trabalho:

- criar um passe `MeshLightRISPresample` ou responsabilidade equivalente em `FMeshLights`;
- preencher o RIS com a alias table atual, guardando índice e inverso da PDF;
- usar seed por frame, tile e entrada, separado dos domínios RNG do ReSTIR DI;
- garantir barreira UAV/SRV antes do Pass A;
- selecionar um tile coerente por grupo 8×8 e amostrar candidatas dentro dele;
- manter o caminho direto pela alias table atrás do toggle A/B;
- preservar o fator da mistura entre pool analítico e pool de triângulos;
- manter oito candidatas durante o A/B estrutural desta fase; a redução de orçamento pertence à
  Fase 2, para não misturar mudança de sampler e quantidade de trabalho no mesmo resultado.

O presample custa 131.072 sorteios por frame. Na Emerald, oito candidatas no DI nativo representam
mais de dez milhões de sorteios globais **e** avaliações de triângulo antes do reuso. Esta fase
remove os sorteios globais do hot path; a Fase 2 localiza o dado e reduz o número de avaliações. A
separação é deliberada para sabermos de onde veio cada ganho.

Gate de correção:

- distribuição observada do RIS compatível com a alias table em teste estatístico offline;
- PDF usada pelo reservoir exatamente igual à probabilidade que gerou a entrada;
- sem NaN/Inf e sem alteração de energia média maior que 1% contra o caminho alias direto após
  acumulação suficiente;
- nenhuma luz dominante desaparece de forma persistente em câmera parada ou em movimento.

Gate de desempenho inicial:

- presample abaixo de 1 ms na RTX 3060 Ti na Emerald;
- nenhum aumento maior que 5% em `Amostragem + temporal` com oito candidatas, contando o novo passe;
- registrar separadamente o ganho que existir, sem exigir 2× antes de compactar o dado ou reduzir
  candidatas;
- nenhum aumento relevante no passe espacial/visibilidade.

Se a variante index-only não ganhar tempo, isso não invalida o RIS: confirma que o fetch do
triângulo e sua avaliação dominam. Seguir para compact data sem aumentar o RIS nem mexer no temporal.

### Fase 2 — localidade do dado e orçamento adaptativo

Objetivo: determinar quanto do custo restante é fetch global e quanto é avaliação por candidata.

Trabalho em dois commits independentes:

1. adicionar o buffer RIS compact com `FTriangleLightGPU` duplicado por entrada;
2. expor orçamento por pool e por preset, sem acoplar ao `InitialCandidates` único atual.

Presets de partida, sujeitos a medida:

- referência científica: 8 analíticas + 8 mesh;
- qualidade: 4 analíticas + 4 mesh;
- gameplay: 2 analíticas + 2 mesh;
- fast: 1 analítica + 1 mesh, com reuso temporal obrigatório.

Não reduzir automaticamente candidatas apenas pela contagem total de triângulos. Fluxo concentrado,
distribuição espacial e movimento de câmera mudam a variância. O preset escolhe orçamento; a
telemetria prova se ele basta.

Gate de saída:

- escolher index-only ou compact data por tempo e VRAM medidos;
- definir defaults por preset;
- obter pelo menos 2× de redução em `Amostragem + temporal` no nativo frente ao baseline limpo,
  contando presample e mantendo o passe espacial estável;
- não aceitar preset que produza boiling persistente, ghosting de emissivos ou perda de luminárias
  pequenas mesmo quando a média global passa.

### Fase 3 — ReSTIR DI em meia resolução

Objetivo: reduzir o fator pixels depois de reduzir o custo por pixel.

Esta fase não substitui o presample. Levar o sampler global atual para meia resolução apenas esconde
parte do custo e deixa o mesmo problema reaparecer em resoluções maiores.

Trabalho:

- reservoirs e dispatch do DI em 1/2 por eixo, não apenas menos candidatas em resolução cheia;
- padrão checkerboard ou seleção alternada dentro do bloco 2×2 para cobrir todos os pixels ao longo
  do tempo;
- reprojeção usando motion confiável na resolução correta;
- upsample bilateral guiado por depth, normal, material e roughness;
- tratamento explícito de silhuetas, emissivos finos, disoclusões e pixels sem geometria;
- integração separada para saída direta crua e para o caminho NRD/DLSS-RR;
- manifesto com `diRenderScaleRequested` e `diRenderScaleEffective`;
- manter nativo como preset científico e fallback.

Gate de saída:

- comparar nativo e meia-res nos mesmos bookmarks da Emerald e Bistro;
- energia média dentro de 1%, PSNR de pelo menos 40 dB no pós-tonemap estabilizado e inspeção visual
  de bordas finas;
- ReSTIR DI total em até 5 ms na 3060 Ti no caso crítico medido, sem deslocar custo equivalente
  para DLSS-RR/NRD ou para o upsample;
- câmera em movimento não pode criar rastro estável em janelas, postes ou letreiros.

O ReSTIR GI em meia resolução é uma trilha irmã e continua pertencendo ao plano SHaRC/WRC. Pode
reusar convenções de coordenadas, checkerboard e upsample desta fase, mas não deve entrar no mesmo
commit do DI.

### Fase 4 — mesh lights no ReGIR e nos hits secundários

Objetivo: fazer mesh lights iluminarem os mesmos hits secundários que hoje recebem apenas sol e
luzes pontuais.

Pré-condições:

- Fases 1 e 2 fechadas;
- captura com ReGIR realmente efetivo, e não apenas solicitado;
- proposta RIS global com PDF validada;
- decisão sobre grade centrada na câmera ou estrutura onion/cascateada.

Trabalho:

- generalizar a identidade de luz do ReGIR para analítica ou triângulo sem quebrar histórico;
- construir células a partir do RIS global, nunca por sorteio uniforme em 228 mil triângulos;
- definir target volumétrico conservador para triângulo, cobrindo toda a célula sem excluir suporte;
- amostrar o ponto baricêntrico somente no shading do hit e incluir sua PDF de área/ângulo sólido;
- avaliar BRDF real no segundo RIS e emitir um único shadow ray para a vencedora;
- substituir a grade esticada pela AABB inteira por uma grade centrada na câmera ou onion antes de
  considerar a Emerald validada;
- ligar o mesmo `PT_AddDirectLocal` a DDGI, reflexos, ReSTIR GI e updater do radiance cache;
- somente depois permitir ReGIR como gerador inicial do ReSTIR DI de tela.

Mesh lights não podem entrar só no updater do radiance cache. Isso quebraria a paridade já
registrada no plano SHaRC: a mesma superfície teria radiância diferente conforme o consumidor.

Gate de correção:

- cena sintética pequena comparada ao loop de referência exato;
- energia média dentro de 1% e sem célula permanentemente vazia onde exista contribuição;
- movimento de câmera e mudança de conjunto de luzes invalidam ou remapeiam histórico corretamente;
- DDGI, reflexos, ReSTIR GI e cache mostram a mesma política de direta local.

Gate de desempenho:

- custo do build não crescer com a resolução de tela;
- custo por hit secundário limitado ao orçamento de propostas e a um shadow ray vencedor;
- ReGIR só roda quando existe consumidor efetivo e luz compatível, preservando o gate atual.

### Fase 5 — emissivos dinâmicos e caminho totalmente GPU

Objetivo: remover as limitações do readback/alias CPU apenas quando conteúdo dinâmico justificar.

Hoje o caminho estático é uma vantagem: extração, cópia e alias só acontecem quando a cena muda. O
readback não é o gargalo observado e não deve ser reescrito por antecipação.

Quando houver transforms ou emissão animada em escala:

- separar pools estático e dinâmico;
- atualizar apenas tasks/triângulos sujos;
- manter buffers anterior/atual para tradução temporal de índices;
- substituir a alias CPU por PDF+mips ou alias GPU se a medida mostrar latência visível;
- nunca publicar `MeshLightCount > 0` enquanto a distribuição correspondente não estiver pronta;
- evitar o intervalo escuro atual durante rebuild usando double buffering da distribuição válida.

Gate de saída:

- editar transform, material emissivo e conjunto de renderables sem descritor obsoleto, tela branca
  ou um frame alternado incorreto;
- custo proporcional ao subconjunto dinâmico, não ao total estático da Emerald.

### Fase 6 — promoção a default

Somente promover quando:

- baselines limpas existirem para Bistro, Emerald e cenas sintéticas;
- os pares requested/effective estiverem no manifesto;
- presets tiverem budgets e fallback documentados;
- testes de distribuição/PDF passarem no CI;
- VRAM adicional estiver no tracker por categoria;
- hot reload, resize, duplicação/remoção de objetos e troca de cena estiverem cobertos;
- a combinação escolhida fechar o budget de frame sem depender de uma única câmera favorável.

## 6. Matriz mínima de validação

### Conteúdo

- zero mesh lights;
- uma área emissiva grande;
- 512 triângulos com potência uniforme;
- 8.192 triângulos com uma luz dominante;
- Bistro, 26.498 triângulos;
- Emerald, 228.374 triângulos;
- cena sintética acima de 262 mil triângulos distribuídos em salas separadas;
- mistura de luzes analíticas e mesh lights;
- emissivos degenerados, fluxo zero, RGB extremo e mapa emissivo.

### Estado temporal

- câmera parada por pelo menos 256 frames;
- translação e rotação contínuas;
- disoclusão forte;
- mudança de intensidade e transform;
- duplicação e remoção de emissivo;
- resize e alternância nativo/meia-res;
- toggle mesh pool, RIS, compact data, ReGIR e denoiser.

### Sinais a registrar

- EMA e distribuição de tempo de `presample`, `Amostragem + temporal`, `Espacial + visibilidade`,
  upsample e denoiser;
- resolução de render e saída;
- candidatas avaliadas por pool;
- entradas válidas no RIS e cells válidas no ReGIR;
- seleção analítica versus mesh;
- reservoir vazio e amostra ocluída;
- energia média, PSNR e mapa de diferença contra referência;
- VRAM por buffer;
- hash limpo e todos os modos efetivos.

## 7. Sequência de commits sugerida

Cada item deve ser reviewável e preservar o A/B anterior.

1. `docs/telemetry: registra baseline efetiva de mesh lights e ReSTIR DI`
2. `feat(mesh-lights): adiciona buffer e passe de presample RIS`
3. `perf(restir-di): amostra mesh lights por tiles RIS coerentes`
4. `perf(mesh-lights): avalia armazenamento compacto no RIS`
5. `perf(restir-di): separa budgets analítico e mesh por preset`
6. `feat(restir-di): adiciona caminho em meia resolução e upsample`
7. `feat(regir): aceita candidatos polimórficos vindos do RIS`
8. `feat(regir): troca AABB global por estrutura centrada na câmera`
9. `feat(mesh-lights): suporta pool dinâmico e distribuição double-buffered`
10. `perf(lighting): promove presets validados e fecha defaults`

Não misturar no mesmo commit:

- mudança de PDF e mudança de resolução;
- compact data e alteração do número de candidatas;
- mesh lights no ReGIR e refactor dos consumidores;
- correção de janela/material e mudança de iluminação;
- otimização e alteração do estimador sem um caminho A/B.

## 8. Decisões que não precisam ser rediscutidas ao retomar

- Mesh lights ficam na engine.
- A alias table por potência é necessária nas cenas atuais e está conceitualmente correta.
- Orçamento único sobre o pool combinado apaga luzes analíticas; os pools permanecem separados.
- A contagem de 228 mil não cria um loop O(N) por pixel no código atual.
- O gargalo observado está no Pass A e escala quase linearmente com pixels.
- RIS presampling vem antes de ReGIR com mesh lights.
- ReGIR atual continua útil apenas para luzes pontuais em hits secundários até a Fase 4.
- Meia resolução vem depois de reduzir o custo por pixel.
- O problema visual das janelas da Emerald é do pipeline de material/cooker e não serve como gate de
  correção de mesh lights.
- Não importar o runtime RTXDI inteiro para resolver esta etapa; adaptar os blocos necessários ao
  renderer e aos contratos existentes da SmileEngine.

## 9. Proveniência técnica

A arquitetura do presample RIS, tiles coerentes, compact light data opcional e ReGIR alimentado por
um pool pré-amostrado vem da implementação RTXDI local. O uso da alias table estática, a extração
somente quando sujo, os pools separados e o protocolo de captura são decisões da SmileEngine.

As referências de Assassin's Creed Shadows, idTech8 e GPU Zen continuam úteis para orçamento de
ray tracing, scheduling e comparação de arquiteturas de GI. Elas não são a fonte primária deste
sampler de mesh lights. O bloco específico deste plano deve ser validado primeiro contra:

- `D:/Engines/RTXDI-main/Doc/Integration.md`, seções de PDF textures, local-light presampling e
  world-space/ReGIR;
- `D:/Engines/RTXDI-main/Doc/ShaderAPI.md`, funções `RTXDI_PresampleLocalLights`,
  `RTXDI_SampleLocalLights` e `RTXDI_PresampleLocalLightsForReGIR`;
- `D:/Engines/RTXDI-main/Libraries/Rtxdi/Include/Rtxdi/LightSampling/PresamplingFunctions.hlsli`;
- `D:/Engines/RTXDI-main/Libraries/Rtxdi/Include/Rtxdi/LightSampling/LocalLightSelection.hlsli`;
- `D:/Engines/RTXDI-main/Samples/FullSample/Shaders/LightingPasses/Presampling/PresampleLights.hlsl`.

Automação da régua:

- `Tools/SmileMCP/README.md`;
- ferramenta MCP `smile_capture_frame`;
- `Tools/SmileMCP/scripts/capture-smoke.mjs` para o smoke do caminho editor → PNG + manifesto.

Arquivos da SmileEngine que formam o ponto de partida:

- `Engine/Include/Smile/Graphics/Lighting/MeshLights.h`;
- `Engine/Source/Graphics/Lighting/MeshLights.cpp`;
- `Engine/Include/Smile/Graphics/Lighting/ReSTIRDI.h`;
- `Engine/Source/Graphics/Lighting/ReSTIRDI.cpp`;
- `Engine/Include/Smile/Graphics/GI/ReGIR.h`;
- `Engine/Source/Graphics/GI/ReGIR.cpp`;
- `Shaders/Lighting/MeshLightExtract.cs.hlsl`;
- `Shaders/Lighting/MeshLightCommon.hlsli`;
- `Shaders/Lighting/ReSTIRDIInitialTemporal.cs.hlsl`;
- `Shaders/Lighting/ReSTIRDISpatial.cs.hlsl`;
- `Shaders/Lighting/DILightSampling.hlsli`;
- `Shaders/GI/ReGIRBuild.cs.hlsl`;
- `Shaders/GI/ReGIRSampling.hlsli`;
- `Shaders/GI/PathTracingCommon.hlsli`.

## 10. Ponto exato para retomar

Ao continuar este trabalho:

1. ler este documento inteiro e o bloco de paridade de mesh lights no plano SHaRC;
2. não começar pelo ReGIR nem pela meia-res;
3. capturar a baseline limpa da Emerald pelo `smile_capture_frame`, com bookmark, N, preset e TOD
   declarados, depois conferir os modos efetivos no manifesto retornado;
4. implementar o RIS mínimo de 1 MiB alimentado pela alias table existente;
5. comparar index-only, depois compact data, sem mudar simultaneamente o orçamento;
6. atualizar o bloco de estado no topo com hash, números e gate fechado antes de abrir a fase
   seguinte.
