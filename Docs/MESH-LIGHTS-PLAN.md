# Mesh lights em escala: plano para ReSTIR DI, ReGIR e meia resolução

> **Estado em 2026-08-15:** plano registrado; nenhuma fase abaixo deve ser lida como implementada.
> O próximo trabalho é a Fase 0, seguida do presample RIS da Fase 1. O `FMeshLights` atual continua
> na engine e o toggle de mesh lights do ReSTIR DI continua sendo a referência A/B.
> A captura da Fase 0 já está automatizada pelo MCP local: `smile_capture_frame` pode restaurar
> bookmark, aquecer frames renderizados, fixar TOD e devolver PNG + manifesto ao chamador.
>
> **Decisão central:** o gargalo da Emerald Square não pede remover mesh lights nem aumentar o
> reservoir. A extração e a distribuição por potência já fazem trabalho útil. O próximo degrau é
> tirar o sorteio da distribuição global de dentro de cada pixel e criar um presample RIS coerente.
> A variante mínima ainda busca o triângulo no pool global; a variante compacta é que remove esse
> segundo acesso aleatório. Só depois esse mesmo pool deve alimentar o ReGIR. DI em meia resolução
> é uma etapa posterior e independente da correção do sampler.

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
pelo protocolo de captura antes de fixar metas absolutas.

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

- `Engine/Include/Smile/Graphics/MeshLights.h`;
- `Engine/Source/Graphics/MeshLights.cpp`;
- `Engine/Include/Smile/Graphics/ReSTIRDI.h`;
- `Engine/Source/Graphics/ReSTIRDI.cpp`;
- `Engine/Include/Smile/Graphics/ReGIR.h`;
- `Engine/Source/Graphics/ReGIR.cpp`;
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
