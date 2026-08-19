# Guia de iluminação global da SmileEngine

Este guia explica o sistema de **Global Illumination (GI)** da SmileEngine para
quem ainda não conhece renderização em tempo real. Ele começa nos conceitos
básicos, apresenta cada subsistema e termina com um mapa do código, ferramentas
de diagnóstico e um roteiro seguro para fazer alterações.

> [!IMPORTANT]
> Este documento descreve o estado implementado na revisão de 2026-08-19.
> Planos e auditorias preservam experimentos históricos; quando houver
> divergência, o código é a fonte de verdade.

## Sumário

1. [GI em 60 segundos](#1-gi-em-60-segundos)
2. [Luz direta e indireta](#2-luz-direta-e-indireta)
3. [Visão geral do sistema](#3-visão-geral-do-sistema)
4. [As três perguntas da política de GI](#4-as-três-perguntas-da-política-de-gi)
5. [Os subsistemas](#5-os-subsistemas)
6. [Como um raio secundário é sombreado](#6-como-um-raio-secundário-é-sombreado)
7. [Ordem no frame](#7-ordem-no-frame)
8. [Defaults e degradação](#8-defaults-e-degradação)
9. [Históricos e invalidação](#9-históricos-e-invalidação)
10. [Denoise e reconstrução temporal](#10-denoise-e-reconstrução-temporal)
11. [Primeiro passeio pelo editor](#11-primeiro-passeio-pelo-editor)
12. [Debug, profiling e capturas](#12-debug-profiling-e-capturas)
13. [Como interpretar performance](#13-como-interpretar-performance)
14. [Mapa de arquivos](#14-mapa-de-arquivos)
15. [Como alterar o sistema com segurança](#15-como-alterar-o-sistema-com-segurança)
16. [Limitações conhecidas](#16-limitações-conhecidas)
17. [Glossário](#17-glossário)
18. [Perguntas frequentes](#18-perguntas-frequentes)

---

## 1. GI em 60 segundos

Sem iluminação indireta, uma parede recebe luz apenas quando uma lâmpada ou o
sol a alcança diretamente. Com GI, a luz pode atingir o chão, refletir na parede
e continuar iluminando o ambiente.

```text
Sem GI

  [luz] ───────────────► [parede iluminada]
                          [canto sem luz]

Com GI

  [luz] ─────► [chão] ─────► [parede] ─────► [canto]
                 1º bounce      2º bounce
```

Calcular todos esses caminhos exatamente é caro. A SmileEngine divide o
problema entre técnicas especializadas:

- **DDGI** guarda iluminação em sondas distribuídas pelo mundo;
- **SHaRC/Radiance Cache** memoriza radiância em uma tabela espacial;
- **ReSTIR GI** escolhe e reutiliza amostras de GI por pixel;
- **ReGIR** ajuda a escolher luzes locais em pontos atingidos por raios;
- **NRD ou DLSS Ray Reconstruction** reduzem o ruído do sinal final.

O resultado é híbrido: nenhuma técnica resolve tudo sozinha.

> [!NOTE]
> Na inicialização apenas com os defaults do C++, o primário pedido é ReSTIR +
> SHaRC, mas o passe ReSTIR GI começa desligado. O estado efetivo degrada para
> DDGI até que o passe seja ligado e esteja pronto. A diferença entre “pedido”
> e “efetivo” é explicada na seção 8.

---

## 2. Luz direta e indireta

### 2.1 Luz direta

É o caminho sem reflexão entre uma fonte e uma superfície:

```text
[lâmpada] ─────────► [mesa]
```

Na SmileEngine, a iluminação direta vem principalmente de:

- sol e lua no deferred lighting;
- luzes point e spot;
- triângulos emissivos, chamados de **mesh lights**;
- ReSTIR DI para selecionar luzes locais de forma eficiente.

Apesar do nome parecido, **ReSTIR DI não é GI**. DI significa *Direct
Illumination*.

### 2.2 Luz indireta

É a energia que já refletiu pelo menos uma vez:

```text
[lâmpada] ─────► [parede vermelha] ─────► [objeto]
                         │
                         └── o objeto recebe parte do vermelho
```

Essa transferência de cor é chamada de *color bleeding*. Outros efeitos
esperados são:

- interiores iluminados por aberturas;
- cantos menos pretos;
- resposta indireta a emissivos;
- reflexos que enxergam ambientes iluminados;
- volumetria recebendo luz do mundo.

### 2.3 Por que existe ruído

Um pixel não consegue testar todos os caminhos possíveis. Ele testa poucas
amostras e varia essas escolhas entre frames. Uma imagem isolada contém ruído;
o histórico temporal, a reutilização espacial e o denoiser convergem o sinal.

```text
Poucas amostras           Reuso temporal/espacial         Imagem reconstruída

 .  * . * .      ─────►      * * * * .         ─────►      ███████████
 * . . * *                    * * * * *                     ███████████
```

Por isso, GI em tempo real deve ser entendida como um sistema **temporal**.

---

## 3. Visão geral do sistema

O diagrama completo de ray tracing está em
[SmileRTArchitecture.svg](SmileRTArchitecture.svg). O recorte abaixo mostra
somente as dependências lógicas de GI:

```text
 CENA
   │
   ├──► BLAS / TLAS / materiais bindless
   └──► G-buffer + depth + motion vectors
              │
              ▼
 ┌──────────────────── DADOS DE MUNDO ────────────────────┐
 │                                                        │
 │  DDGI                         SHaRC / Radiance Cache    │
 │  grade de sondas              hash espacial            │
 │  fallback estável             terminador rápido        │
 │       │                              │                 │
 │       └──────────────┬───────────────┘                 │
 │                      ▼                                 │
 │                 ReSTIR GI                              │
 │          final gather difuso por pixel                 │
 └──────────────────────┬─────────────────────────────────┘
                        │
                        ├──► NRD RELAX, DLSS-RR ou sinal cru
                        ▼
              Deferred lighting / HDR
                        │
                        ▼
                  TAA ou upscaler
```

Dois sistemas relacionados ficam ao lado desse fluxo:

```text
Mesh lights ──► ReSTIR DI ──► iluminação direta local

Luzes locais ──► ReGIR ─────► seleção de luz no hit secundário
```

### Regra mental

- **ReSTIR GI** produz o sinal difuso por pixel.
- **SHaRC** tenta responder rapidamente aos hits secundários.
- **DDGI**, nos raios secundários compartilhados, responde quando o cache não
  pode responder; separadamente, ainda atende volumetria e superfícies
  auxiliares.
- **ReGIR** escolhe luzes, mas não substitui nenhum dos três.

O desenho não representa uma agenda serial. O DDGI pode atualizar em compute
async enquanto a fila direta produz depth, G-buffer e o update do cache; os
consumidores esperam a fence somente antes de ler o atlas.

---

## 4. As três perguntas da política de GI

O arquivo
[`IndirectPolicy.h`](../Engine/Include/Smile/Graphics/IndirectPolicy.h)
existe para impedir que três perguntas diferentes sejam confundidas.

### 4.1 O volume DDGI deve rodar?

Essa é uma pergunta de **orçamento**:

```text
UseGI && DDGI pronto  ──► traçar e atualizar as sondas
```

`UseGI` não significa “todo o sistema de GI está ligado”. Ele controla o
volume DDGI.

### 4.2 Quem produz o indireto de superfície?

Essa é a escolha do **primário**:

| Primário pedido | Resultado desejado |
|---|---|
| `ReSTIR_SHaRC` | ReSTIR GI por pixel, usando SHaRC nos hits elegíveis |
| `DDGI` | atlas de sondas como sinal principal |
| `Off` | sem indireto de superfície |

### 4.3 Quem responde quando o primário não consegue?

Essa é a escolha do **fallback** do raio:

| Fallback pedido | Resultado |
|---|---|
| `DDGI` | consulta o volume de sondas, se disponível |
| `Black` | retorna zero |
| `Environment` | declarado, mas ainda degrada para `Black` |

### 4.4 Pedido não é a mesma coisa que resultado efetivo

A política é resolvida uma vez no início de cada frame:

```text
ReSTIR + SHaRC pedido
        │
        ├── ReSTIR GI pronto ─────────────► ReSTIR + SHaRC efetivo
        │
        ├── ReSTIR indisponível + DDGI ───► DDGI efetivo
        └── nenhum disponível ────────────► Off
```

O manifesto de captura registra o valor **pedido** e o **efetivo**. Essa
diferença é importante: uma opção pode estar selecionada, mas degradar porque o
passe, o volume ou um SDK não está disponível.

### 4.5 O terceiro papel do DDGI

Mesmo com ReSTIR + SHaRC como primário, o atlas DDGI ainda ilumina:

- o preenchimento de folhagem;
- o termo traseiro de materiais subsurface;
- translúcidos no ForwardBlend;
- a névoa volumétrica.

Portanto:

> `fallback = Black` não significa “nenhum DDGI na imagem”.

Hoje não existe um toggle que mantenha DDGI apenas na névoa e o remova de todos
os auxiliares de superfície.

---

## 5. Os subsistemas

### 5.1 DDGI: uma grade de sensores de luz

DDGI significa *Dynamic Diffuse Global Illumination*. Imagine uma grade 3D de
pequenos sensores:

```text
        ○────○────○────○
       /│   /│   /│   /│
      ○────○────○────○ │
      │ ○──│─○──│─○──│─○
      │/   │/   │/   │/
      ○────○────○────○

      ○ = probe / sonda
```

Cada sonda:

1. dispara raios em várias direções;
2. encontra superfícies da cena pela TLAS;
3. calcula a radiância desses hits;
4. acumula irradiância e distância em atlases;
5. pode ser consultada depois por superfícies próximas.

#### O que o DDGI armazena

- **Irradiance atlas:** quanta luz difusa chega de cada direção;
- **distance moments:** distância média/variância para reduzir vazamento;
- **probe data:** deslocamento e classificação das sondas;
- **ray count:** orçamento usado por cada sonda.

#### Histerese

O atlas não troca de valor de uma vez. Ele mistura o frame novo com o histórico:

```text
valor = histórico × histerese + amostra_nova × (1 - histerese)
```

Com histerese alta, o sinal fica estável, mas reage devagar. Os defaults são:

- irradiância: até `0.98`;
- distância: `0.99`.

É por isso que esquecer uma invalidação pode deixar luz antiga visível por
muitos frames.

#### Cascatas

O código suporta uma grade grossa e uma fina:

```text
┌──────────────── volume grosso da cena ────────────────┐
│                                                       │
│             ┌── volume fino perto da câmera ──┐       │
│             │              câmera             │       │
│             └─────────────────────────────────┘       │
└───────────────────────────────────────────────────────┘
```

O default atual pede **uma cascata**. A segunda cascata e o scrolling toroidal
estão implementados, mas a auditoria desta branch ainda não registra todos os
gates de runtime como concluídos.

#### Defaults importantes

| Parâmetro | Default |
|---|---:|
| Raios por sonda | 64 |
| Cascatas pedidas | 1 |
| Relocação | ligada |
| Raios adaptativos | desligados |
| Histerese adaptativa | desligada |
| Escala do bias de superfície | 0,2 |
| Teto do bias | 0,40 m |
| Fade na borda | 1 célula |

Código: [`DDGI.h`](../Engine/Include/Smile/Graphics/DDGI.h) e shaders
[`DDGI*.hlsl`](../Shaders/GI/).

### 5.2 SHaRC / Radiance Cache: memória espacial

O Radiance Cache guarda respostas em uma tabela hash no espaço do mundo.

```text
posição + normal + LOD
          │
          ▼
      função hash
          │
          ▼
┌─────────┬─────────┬─────────┬─────────┐
│ célula  │ célula  │ célula  │ célula  │
│ vazia   │ Lo=...  │ Lo=...  │ vazia   │
└─────────┴─────────┴─────────┴─────────┘
```

Quando um raio encontra uma superfície, o cache tenta responder antes de fazer
o trabalho caro de material, luzes, shadow rays e gather DDGI.

#### Fast path

```text
hit secundário
      │
      ├── cache válido ──► retorna radiância imediatamente
      │
      └── miss ─────────► shading completo + fallback
```

Esse retorno antecipado está em
[`HitShading.hlsli`](../Shaders/GI/HitShading.hlsli). A ordem é parte do
contrato de performance.

#### Quem alimenta o cache

O caminho padrão usa um updater dedicado depois do G-buffer:

```text
G-buffer
   │
   └──► amostra uma fração dos pixels
           │
           └──► traça caminhos curtos
                    │
                    └──► atualiza o hash de radiância
```

Defaults atuais:

| Parâmetro | Default |
|---|---:|
| Capacidade | `2^17` entradas |
| Tamanho base da célula | 0,50 m |
| Distância do primeiro LOD | 6 m |
| Fração atualizada por frame | 4% |
| Vértices máximos por caminho | 1 |
| Amostras mínimas para consulta | 4 |
| Updater dedicado | ligado |
| Scheduling compacto | ligado |
| Auto-warmup | ligado |

Código: [`RadianceCache.h`](../Engine/Include/Smile/Graphics/RadianceCache.h),
[`RadianceCache.hlsli`](../Shaders/GI/RadianceCache.hlsli) e
[`RadianceCacheUpdate.cs.hlsl`](../Shaders/GI/RadianceCacheUpdate.cs.hlsl).

### 5.3 ReSTIR GI: reutilização inteligente por pixel

ReSTIR não tenta calcular muitas amostras em cada pixel. Ele mantém um
**reservoir**, uma estrutura compacta que representa uma amostra escolhida e o
peso estatístico da escolha.

```text
amostra nova ─────────────┐
                         ▼
reservoir do frame anterior ──► seleção ponderada ──► reservoir atual
                         ▲
reservoirs vizinhos ─────┘
```

O passe possui duas fases principais:

1. **Trace/temporal:** gera a amostra e tenta reutilizar o frame anterior;
2. **Spatial:** consulta vizinhos e melhora a distribuição.

Defaults:

- reutilização temporal ligada;
- reutilização espacial ligada;
- visibility rays espaciais desligados por custo;
- histórico em ping-pong;
- sinal cru por default, salvo quando NRD está efetivamente ativo.

ReSTIR GI produz o sinal principal de superfície somente quando a política
efetiva é `ReSTIR_SHaRC`.

Código: [`ReSTIRGI.h`](../Engine/Include/Smile/Graphics/ReSTIRGI.h),
[`ReSTIRGITrace.cs.hlsl`](../Shaders/GI/ReSTIRGITrace.cs.hlsl) e
[`ReSTIRGISpatial.cs.hlsl`](../Shaders/GI/ReSTIRGISpatial.cs.hlsl).

### 5.4 ReGIR: luzes locais nos hits secundários

Uma superfície atingida por um raio ainda precisa avaliar luzes locais. Fazer
um loop por todas elas é caro. O ReGIR constrói reservoirs em uma grade de
mundo:

```text
muitas luzes locais
        │
        ▼
 grade ReGIR 16 × 8 × 16
        │
        ▼
poucas candidatas relevantes para o hit
```

Ele é consumido por DDGI, ReSTIR GI, reflexões e pelo updater do cache.

O default é **desligado** para permitir A/B. Desligá-lo não remove as luzes
locais: o shader usa o loop completo como referência. ReGIR é uma estratégia
de amostragem, não o interruptor das luzes.

Mesmo ligado, o build só acontece quando existe ao menos um consumidor e uma
luz puntual elegível. Uma cena composta apenas por emissivos não ativa ReGIR:
esses triângulos seguem pelo caminho mesh lights → ReSTIR DI.

Código: [`ReGIR.h`](../Engine/Include/Smile/Graphics/ReGIR.h) e
[`ReGIRBuild.cs.hlsl`](../Shaders/GI/ReGIRBuild.cs.hlsl).

### 5.5 Mesh lights e ReSTIR DI

Triângulos emissivos podem atuar como luzes de área. A engine:

1. extrai os triângulos emissivos;
2. descarta o suporte com fluxo zero;
3. cria uma alias table para sorteio proporcional à energia;
4. copia a tabela para VRAM;
5. entrega as candidatas ao ReSTIR DI.

```text
malhas emissivas
      │
      ▼
triângulos com fluxo > 0
      │
      ▼
alias table em VRAM
      │
      ▼
ReSTIR DI
```

Mesh lights participam da **iluminação direta local**. Elas afetam o sistema de
GI porque a radiância dos hits também contém luz direta, mas não substituem
DDGI, SHaRC ou ReSTIR GI.

Detalhes e medições:
[MESH-LIGHTS-PLAN.md](MESH-LIGHTS-PLAN.md).

---

## 6. Como um raio secundário é sombreado

O núcleo compartilhado está em
[`HitShading.hlsli`](../Shaders/GI/HitShading.hlsli).

```text
Raio acerta uma superfície
          │
          ▼
Carrega posição e normal geométrica
          │
          ▼
Consulta SHaRC
     ┌────┴────┐
     │         │
   HIT        MISS
     │         │
     │         ├──► carrega material
     │         ├──► calcula sol e luzes locais / ReGIR
     │         ├──► consulta fallback DDGI ou zero
     │         ├──► adiciona emissivo
     │         └──► só escreve no cache se o produtor legado estiver ligado
     │
     └────────────► retorna radiância
```

No default, o produtor legado está desligado: o cache é alimentado pelo updater
dedicado pós-G-buffer. A escrita em hits de render permanece apenas como braço
de A/B.

### 6.1 Por que o cache vem antes do material

O cache existe para evitar o caminho caro. Mover a leitura de material para
antes da consulta manteria a imagem correta, mas destruiria parte importante
da economia.

### 6.2 Cache miss e cache inelegível

Nem todo miss significa “a tabela ainda está fria”. Um raio pode ser
geometricamente inelegível por:

- segmento curto demais para a célula;
- cone especular estreito demais;
- célula sem amostras suficientes;
- ausência ou colisão de entrada válida.

Quando isso acontece, o DDGI pode responder. “Por que o cache não respondeu” e
“quem forneceu a radiância” são perguntas diferentes.

### 6.3 Fonte do resultado

A telemetria separa:

```text
CACHE + DDGI + ZERO = total de hits sombreados
```

Na medição registrada da Bistro exterior, com a instrumentação apropriada, a
divisão foi aproximadamente:

- 67,94% cache;
- 30,14% DDGI;
- 1,92% zero.

Esses números pertencem àquela cena, câmera, build e regime de instrumentação.
Não são uma constante universal da engine nem devem ser reproduzidos fora da
série de auditoria de agosto de 2026 sem uma nova captura determinística.

---

## 7. Ordem no frame

Uma versão simplificada da ordem executável:

```text
ANTES DO BEGIN FRAME
  │
  ├─ reconcilia mudanças de materiais, luzes e estrutura de cena
  └─ processa pedidos da captura determinística

INÍCIO DA GRAVAÇÃO
  │
  ├─ resolve política indireta efetiva
  ├─ detecta mudanças e invalida históricos
  ├─ coleta stats e avança o warmup do radiance cache
  ├─ posiciona cascatas DDGI
  ├─ atualiza céu, atmosfera, volumetria e oceano
  ├─ grava céu e nuvens
  │
  ├─ prepara iluminação indireta
  │    ├─ extrai mesh lights quando necessário
  │    ├─ constrói ReGIR quando há consumidor e luz elegível
  │    └─ inicia DDGI em compute async ou executa na fila direta
  │
  ├─ empacota luzes, monta draw lists e grava sombras
  ├─ depth prepass
  ├─ G-buffer
  ├─ updater dedicado do radiance cache
  ├─ espera DDGI async apenas quando necessário
  │
  ├─ ReSTIR GI, se for o primário efetivo
  ├─ reflexões RT antes do deferred SOMENTE no caminho NRD indireto
  ├─ pack GI/reflexões + NRD indireto, quando ativo
  ├─ ReSTIR DI
  ├─ NRD direto, quando ativo
  ├─ deferred lighting
  ├─ resolve do radiance cache
  │
  ├─ sem NRD indireto: trace/composite de reflexões acontece aqui
  ├─ água, translúcidos e nuvens
  ├─ fog, volumetria e chuva
  ├─ TAA / DLSS-RR / upscaler
  └─ pós-processamento e tonemap
```

A posição das reflexões muda conforme o regime. Com NRD indireto, GI e
reflexões precisam ser traçados antes do pack/denoise. Sem NRD indireto — que
inclui o cold start com primário DDGI — as reflexões são compostas depois do
deferred.

O fluxo está em
[`Renderer.cpp`](../Engine/Source/Graphics/Renderer.cpp), principalmente em:

- `ResolveIndirectPolicy`;
- `PrepareIndirectLighting`;
- `RecordSceneLighting`;
- `RenderFrame`.

### Async compute não significa custo zero

O DDGI pode rodar em uma fila compute paralela ao raster:

```text
tempo ─────────────────────────────────────────────►

fila direta   [sombras][depth][G-buffer]──────[lighting]
fila compute       [--------- DDGI ---------]
                                      ▲
                                      wait se ainda não terminou
```

Uma espera pequena mostra bom overlap, mas o DDGI ainda disputa execução,
cache e banda com a fila direta. O número importante para orçamento é o tempo
total do frame, não apenas o scope de espera.

---

## 8. Defaults e degradação

### 8.1 Cold start do C++

Em uma instância criada apenas com os defaults de `Renderer.h`, antes de
presets ou estado persistido pelo editor:

| Opção | Default |
|---|---:|
| Volume DDGI (`UseGI`) | ligado |
| Primário pedido | `ReSTIR_SHaRC` |
| Fallback pedido | `DDGI` |
| Passe ReSTIR GI | desligado |
| ReGIR | desligado |
| Reflexões RT | ligadas |
| Denoiser | nenhum |
| TAA pedido (`UseTAA`) | ligado |
| Upscaler pedido | FSR, com fallback para `None` se indisponível |
| Radiance cache write/query | ligados |

Isso cria uma armadilha importante:

> O primário **pedido** é ReSTIR + SHaRC, mas, com o passe ReSTIR GI
> desligado no cold start, o primário **efetivo** degrada para DDGI.

O cache ainda pode participar de outros traces, como reflexões e atualização
das sondas. Para produzir ReSTIR GI na tela, o passe precisa estar ligado e
pronto.

O TAA próprio só fica efetivamente ativo quando não existe upscaler ativo. Se o
FSR inicializar, ele assume a reconstrução temporal e `TAAActive` fica falso.

### 8.2 Tabela de degradação

| Pedido | Capacidade disponível | Efetivo |
|---|---|---|
| ReSTIR + SHaRC | ReSTIR GI pronto | ReSTIR + SHaRC |
| ReSTIR + SHaRC | ReSTIR indisponível, DDGI vivo | DDGI |
| ReSTIR + SHaRC | nenhum disponível | Off |
| DDGI | DDGI vivo | DDGI |
| DDGI | DDGI indisponível | Off |
| Off | qualquer | Off |

Fallback:

| Pedido | Condição | Efetivo |
|---|---|---|
| DDGI | volume vivo | DDGI |
| DDGI | volume indisponível | Black |
| Environment | qualquer | Black |
| Black | qualquer | Black |

### 8.3 Recursos neutros

ReSTIR GI e reflexões podem existir mesmo sem um volume DDGI. Para manter as
tabelas de descriptors válidas, a engine usa recursos neutros 1×1 zerados e
fecha a leitura por uma flag no shader.

Esse contrato está em
[`GIFallback.h`](../Engine/Include/Smile/Graphics/GIFallback.h).

---

## 9. Históricos e invalidação

### 9.1 Por que invalidar

Imagine que um reservoir acumulou iluminação de uma lâmpada azul. Se a lâmpada
vira vermelha e o histórico não é invalidado, frames seguintes misturam as
duas situações.

```text
histórico azul + estado vermelho = resultado temporariamente incorreto
```

### 9.2 Principais históricos

| Sistema | Histórico |
|---|---|
| DDGI | atlases de irradiância/distância |
| ReSTIR GI | reservoirs ping-pong |
| ReGIR | reservoirs da grade |
| Radiance Cache | hash de mundo e médias |
| Reflexões | acumulação temporal |
| NRD | histórico do denoiser |
| Temporal Motion | superfície e transforms anteriores |
| TAA / DLSS-RR | reconstrução de tela |

### 9.3 HistoryDomain

[`HistoryDomain.h`](../Engine/Include/Smile/Graphics/HistoryDomain.h) define 17
alvos e máscaras nomeadas pelo **motivo** da mudança.

Exemplos:

| Evento | Domínio / comportamento |
|---|---|
| Mudou o fallback | `IndirectTerminator` |
| Mudou o primário | `IndirectSurfaceRoute` |
| Volume apareceu/sumiu | `IndirectVolumetricSource` |
| Mudou material visto por RT | `MaterialRTState` |
| Objeto nasceu/morreu | domínio `SceneStructure`; DDGI usa `InvalidateRegion` separadamente |
| Luz ou objeto foi editado | domínio `SceneContent`; DDGI usa invalidação regional |
| Câmera teleportou | `CameraCut` |
| Captura científica iniciou | `DeterministicCapture` |

### 9.4 Camera cut não é reset científico

Mover ou restaurar a câmera não deve apagar caches de mundo: uma sonda continua
representando o mesmo lugar.

```text
CameraCut
  └── reseta históricos de tela e reprojeção

DeterministicCapture
  └── reseta TUDO, inclusive DDGI, ReGIR e Radiance Cache
```

Essa diferença é essencial para comparações reproduzíveis.

### 9.5 Invalidação regional

Criar um objeto não precisa apagar todas as sondas. A engine invalida a região
afetada no DDGI. Isso evita trocar 99% de um atlas correto por amostras novas de
um único frame.

---

## 10. Denoise e reconstrução temporal

GI por poucos raios é ruidosa. A engine oferece três caminhos principais.

### 10.1 Sem denoiser

Quando o primário efetivo é ReSTIR + SHaRC, o ReSTIR GI cru segue para o
deferred. Quando o primário efetivo é DDGI, o deferred lê o atlas de sondas em
vez da textura do ReSTIR GI. O TAA ainda pode estabilizar a imagem final, mas
não conhece toda a semântica do sinal de GI.

### 10.2 NRD RELAX

O NRD recebe sinais e guides específicos:

```text
GI difuso + reflexão especular + depth + normal + motion
                         │
                         ▼
                    NRD RELAX
                         │
                         ▼
                 sinais reconstruídos
```

A engine mantém instâncias separadas para:

- indireto: GI e reflexões;
- direto: ReSTIR DI.

Os dois sinais possuem distribuições de ruído diferentes.

O NRD indireto só executa quando o primário efetivo é ReSTIR + SHaRC, o NRD
está pronto e o modo selecionado é NRD. Com primário DDGI, o atlas continua
cru e as reflexões seguem o caminho próprio pós-deferred.

### 10.3 DLSS Ray Reconstruction

DLSS-RR combina denoise e upscale em uma avaliação. Quando selecionado, ele
força o caminho DLSS e usa guides de material, depth e motion.

Uma visualização de debug pode substituir o HDR por dados artificiais. Nesse
caso, o eval de RR é pulado para não entregar um sinal inválido à rede.

### 10.4 Motion vector confiável

TAA e upscalers usam o velocity raster. ReSTIR e NRD também precisam acompanhar
o efeito de iluminação que se move em hits secundários. Para isso existe um
vetor dual, explicado em
[TEMPORAL_MOTION_VECTORS.md](TEMPORAL_MOTION_VECTORS.md).

Não troque um pelo outro: eles respondem perguntas diferentes.

---

## 11. Primeiro passeio pelo editor

Os controles ficam em **Configurações → Iluminação global**, no card
**Pipeline de iluminação indireta**.

> [!WARNING]
> Não altere vários eixos ao mesmo tempo. GI acumula estado; uma comparação
> sem convergência pode mostrar o histórico anterior em vez da opção atual.

### Experimento 1: entender DDGI

1. Ligue o volume DDGI.
2. Selecione primário `DDGI`.
3. Deixe ReSTIR GI desligado.
4. Observe interiores, cantos e o debug do volume.
5. Desligue o primário de superfície, sem confundir com o volume.

O objetivo é enxergar o que o atlas entrega diretamente.

### Experimento 2: ReSTIR + SHaRC

1. Ligue ReSTIR GI.
2. Selecione primário `ReSTIR + SHaRC`.
3. Mantenha fallback `DDGI`.
4. Aguarde o cache sair do warmup.
5. Compare o sinal cru e o denoisado.

### Experimento 3: medir o fallback

1. Mantenha ReSTIR + SHaRC como primário.
2. Compare fallback `DDGI` e `Preto`.
3. Espere os históricos reconvergirem.
4. Observe interiores e superfícies voltadas contra a luz.

Isso mede a importância do terminador DDGI, não remove todos os usos auxiliares
do atlas.

### Experimento 4: ReGIR

1. Use uma cena com muitas luzes locais.
2. Compare ReGIR desligado e ligado.
3. Observe reflexões e GI, não apenas a iluminação direta da câmera.
4. Compare o profiler, mantendo o restante fixo.

### Controles de laboratório

O editor também expõe:

- write/query do radiance cache;
- stats e telemetria de fonte;
- fração do updater;
- tamanho de célula e LOD;
- cascatas e bias DDGI;
- raios e histerese adaptativos;
- mapa de fonte do ReSTIR GI;
- epsilons globais de raio.

Esses controles são instrumentos de engenharia, não presets de qualidade.

---

## 12. Debug, profiling e capturas

### 12.1 Debug targets

Abra a janela de render targets para inspecionar:

- ReSTIR GI cru;
- saída NRD;
- reflexões;
- guides de DLSS-RR;
- radiance cache;
- mapa da fonte do candidato;
- probes e volume DDGI.

Um target preto pode significar:

- passe efetivamente desligado;
- warmup ainda em andamento;
- fallback efetivo diferente do pedido;
- descriptor neutro usado de propósito;
- visualização incompatível com o modo atual.

### 12.2 GPU profiler

Scopes úteis:

- `DDGI` ou `DDGI (async)`;
- `Espera do DDGI (async)`;
- `Radiance cache (update)`;
- `Radiance cache (resolve)`;
- `ReSTIR GI`;
- `Reflexos (trace)`;
- `Reflexos (composite)`;
- `Pack GI + reflexos`;
- `NRD denoise`;
- `NRD direta`;
- `ReSTIR DI`;
- `MeshLights (extract)`;
- `ReGIR (build)`.

Os scopes presentes variam conforme a política e o denoiser efetivos. Leia o
custo do passe junto com o **frame GPU total**.

### 12.3 Captura determinística

Para um A/B sério:

1. salve ou escolha um bookmark de câmera;
2. aplique o preset científico;
3. fixe o horário e os knobs;
4. use captura determinística;
5. aqueça os 128 frames calibrados;
6. compare PNG e manifesto;
7. mantenha o mesmo regime de instrumentação.

O protocolo completo está em
[CAPTURE-PROTOCOL.md](CAPTURE-PROTOCOL.md). A automação está em
[Tools/SmileMCP](../Tools/SmileMCP/README.md).

### 12.4 Instrumentação não é gratuita

Stats detalhados e telemetria de fonte acrescentam atomics e podem alterar o
comportamento do cache. Não compare uma captura limpa com outra usando stats
detalhados.

---

## 13. Como interpretar performance

### 13.1 Quatro custos diferentes

Não reduza GI a um único número:

| Custo | Exemplo |
|---|---|
| Atualização de mundo | DDGI e updater SHaRC |
| Trace por pixel | ReSTIR GI e reflexões |
| Reconstrução | NRD ou DLSS-RR |
| Sincronização/contenção | overlap entre compute e fila direta |

### 13.2 Cena e câmera fazem parte da medição

Uma cena externa pode favorecer cache espacial. Um interior com paredes finas
pode expor vazamento do DDGI. Uma cena com muitos emissivos muda ReSTIR DI.

Sempre registre:

- cena e bookmark;
- resolução;
- GPU;
- build/commit;
- preset;
- horário;
- warmup;
- instrumentação;
- eixo que mudou.

### 13.3 PNG não é energia HDR

As capturas atuais são pós-tonemap. Uma diferença de luminância em PNG é uma
diferença de imagem exibida, não uma medida linear de energia luminosa.

### 13.4 Números históricos úteis, mas não universais

- A calibração de captura escolheu `N = 128`.
- Na Bistro medida, o fallback DDGI respondeu 30,14% dos hits secundários.
- A segunda cascata fixa melhorou vazamento por custo medido de frame.
- Mover a alias table de mesh lights para VRAM removeu o maior gargalo daquela
  série na Emerald.

Consulte os documentos de auditoria antes de citar valores:

- [SHARC-PRIMARY-GI-PLAN.md](SHARC-PRIMARY-GI-PLAN.md)
- [DDGI-AUDIT.md](DDGI-AUDIT.md)
- [MESH-LIGHTS-PLAN.md](MESH-LIGHTS-PLAN.md)

---

## 14. Mapa de arquivos

### Contratos C++

| Arquivo | Responsabilidade |
|---|---|
| [`IndirectPolicy.h`](../Engine/Include/Smile/Graphics/IndirectPolicy.h) | primário, fallback e estado efetivo |
| [`DDGI.h`](../Engine/Include/Smile/Graphics/DDGI.h) | volume, cascatas, atlases e knobs |
| [`ReSTIRGI.h`](../Engine/Include/Smile/Graphics/ReSTIRGI.h) | reservoirs e passes GI |
| [`RadianceCache.h`](../Engine/Include/Smile/Graphics/RadianceCache.h) | hash, updater, warmup e stats |
| [`ReGIR.h`](../Engine/Include/Smile/Graphics/ReGIR.h) | grade de reservoirs de luz |
| [`GIHitSampling.h`](../Engine/Include/Smile/Graphics/GIHitSampling.h) | parâmetros compartilhados do hit |
| [`GIFallback.h`](../Engine/Include/Smile/Graphics/GIFallback.h) | descriptors neutros e bindings |
| [`HistoryDomain.h`](../Engine/Include/Smile/Graphics/HistoryDomain.h) | grafo de invalidação |
| [`RenderSettings.h`](../Engine/Include/Smile/Graphics/RenderSettings.h) | API dos controles |
| [`Renderer.cpp`](../Engine/Source/Graphics/Renderer.cpp) | orquestração do frame |

### Shaders

| Arquivo | Responsabilidade |
|---|---|
| [`HitShading.hlsli`](../Shaders/GI/HitShading.hlsli) | cache → direta → fallback → emissivo |
| [`DDGICommon.hlsli`](../Shaders/GI/DDGICommon.hlsli) | gather, cascatas e Chebyshev |
| [`DDGITrace.cs.hlsl`](../Shaders/GI/DDGITrace.cs.hlsl) | trace das sondas |
| [`DDGIUpdate.cs.hlsl`](../Shaders/GI/DDGIUpdate.cs.hlsl) | atlas de irradiância |
| [`DDGIUpdateDist.cs.hlsl`](../Shaders/GI/DDGIUpdateDist.cs.hlsl) | momentos de distância |
| [`DDGIRelocate.cs.hlsl`](../Shaders/GI/DDGIRelocate.cs.hlsl) | relocação e classificação |
| [`RadianceCache.hlsli`](../Shaders/GI/RadianceCache.hlsli) | query/update do hash |
| [`RadianceCacheUpdate.cs.hlsl`](../Shaders/GI/RadianceCacheUpdate.cs.hlsl) | produtor dedicado |
| [`RadianceCacheResolve.cs.hlsl`](../Shaders/GI/RadianceCacheResolve.cs.hlsl) | resolve e stats |
| [`ReSTIRGITrace.cs.hlsl`](../Shaders/GI/ReSTIRGITrace.cs.hlsl) | trace e temporal |
| [`ReSTIRGISpatial.cs.hlsl`](../Shaders/GI/ReSTIRGISpatial.cs.hlsl) | reuso espacial |
| [`ReSTIRReservoir.hlsli`](../Shaders/GI/ReSTIRReservoir.hlsli) | operações do reservoir |
| [`ReGIRBuild.cs.hlsl`](../Shaders/GI/ReGIRBuild.cs.hlsl) | construção da grade |
| [`ReGIRSampling.hlsli`](../Shaders/GI/ReGIRSampling.hlsli) | amostragem no hit |
| [`DeferredLighting.ps.hlsl`](../Shaders/DeferredLighting.ps.hlsl) | composição de superfície |

### Editor e ferramentas

| Arquivo | Responsabilidade |
|---|---|
| [`SettingsWindow.qml`](../Editor/Qml/SettingsWindow.qml) | controles e laboratório GI |
| [`DebugTargetsWindow.qml`](../Editor/Qml/DebugTargetsWindow.qml) | inspeção dos buffers |
| [`RenderSettingsBridge.h`](../Editor/Include/SmileEditor/RenderSettingsBridge.h) | bridge QML |
| [`CAPTURE-PROTOCOL.md`](CAPTURE-PROTOCOL.md) | contrato de comparação |
| [`Tools/SmileMCP`](../Tools/SmileMCP/README.md) | automação de build, profiling e captura |

---

## 15. Como alterar o sistema com segurança

### 15.1 Adicionar um knob

1. defina o estado no subsistema apropriado;
2. exponha por `FRenderSettings`;
3. classifique o histórico em `HistoryDomain`;
4. adicione o bridge e o controle QML;
5. use guarda de valor igual;
6. para sliders caros, aplique na liberação ou use dirty coalescido;
7. capture antes/depois com a mesma configuração.

Pergunta obrigatória:

> Este valor muda algo que será acumulado em outro frame?

Se sim, ele precisa invalidar o histórico correspondente.

### 15.2 Alterar `HitShading`

Esse arquivo possui vários consumidores: DDGI, ReSTIR GI, reflexões e água.

Checklist:

- a consulta ao cache continua antes do caminho caro?
- o fallback mudou para todos os consumidores?
- o atlas DDGI também usa esse terminador no segundo bounce?
- a telemetria CACHE/DDGI/ZERO ainda fecha no total?
- os domínios `IndirectTerminator` ou `RayVisibility` precisam mudar?
- o manifesto descreve o estado efetivo?

### 15.3 Adicionar um histórico

1. adicione um bit em `EHistoryTarget`;
2. atualize `kHistoryTargetCount`;
3. inclua o alvo nos domínios corretos;
4. implemente a invalidação no executor;
5. inclua em `DeterministicCapture`;
6. verifique camera cut separadamente;
7. registre o alvo no contrato do passe.

### 15.4 Alterar um constant buffer

Layouts C++ e HLSL são espelhados manualmente. Campos novos devem ser
acrescentados no fim quando possível, com `static_assert` de tamanho e offset.

Um layout errado pode compilar e produzir dados silenciosamente corrompidos.

### 15.5 Adicionar um shader

1. adicione o `.hlsl` em `Shaders/GI/`;
2. registre em `Shaders/CMakeLists.txt`;
3. declare o stem em `ShaderStems()`;
4. implemente `OnRecreatePipelines()`;
5. valide Debug e Release;
6. teste hot-reload.

---

## 16. Limitações conhecidas

### Implementado com validação pendente ou parcial

- scrolling da cascata fina do DDGI;
- comportamento de múltiplas cascatas em todos os movimentos e teleportes;
- raios e histerese adaptativos permanecem desligados por default;
- ReGIR está implementado, mas desligado por default;
- o caminho principal de mesh lights para DI — extração, compactação, alias em
  VRAM e amostragem no ReSTIR DI — está implementado; telemetria e integrações
  posteriores descritas no plano continuam abertas.

### Ainda não implementado

- fallback de ambiente real para raios secundários;
- estado “SHaRC sem auxiliares DDGI de superfície, mantendo DDGI na névoa”;
- orçamento final do DDGI por cascata;
- todos os gates de runtime da Fase 6 do plano SHaRC;
- um frame graph declarativo completo.

### Limitações estruturais

- `Renderer` ainda concentra a integração dos subsistemas;
- layouts C++/HLSL não usam um header compartilhado;
- GI depende de DirectX 12/DXR e validação real em GPU;
- planos antigos podem descrever baselines, não os defaults atuais.

---

## 17. Glossário

| Termo | Explicação |
|---|---|
| **GI** | iluminação que refletiu ao menos uma vez |
| **Radiância** | energia luminosa viajando em uma direção |
| **Irradiância** | energia total chegando a uma superfície |
| **Bounce** | uma reflexão do caminho de luz |
| **Probe / sonda** | amostrador de iluminação colocado no mundo |
| **Atlas** | textura que empacota dados de muitas sondas |
| **DDGI** | GI dinâmica difusa por grade de sondas |
| **SHaRC/WRC** | cache de radiância em hash no espaço do mundo |
| **ReSTIR** | reutilização de amostras com reservoirs |
| **Reservoir** | representação compacta de uma seleção ponderada |
| **ReGIR** | reservoirs de luz em uma grade do mundo |
| **DI** | iluminação direta |
| **Mesh light** | triângulo emissivo tratado como luz |
| **RayQuery** | ray tracing inline dentro de um shader |
| **BLAS** | aceleração da geometria de uma malha |
| **TLAS** | aceleração das instâncias da cena |
| **Fallback** | resposta usada quando o caminho principal falha |
| **Denoiser** | filtro especializado em remover ruído de RT |
| **Histerese** | peso dado ao histórico na atualização |
| **Temporal** | informação acumulada entre frames |
| **Spatial** | reutilização entre pixels ou regiões vizinhas |
| **Warmup** | período para preencher e estabilizar um cache |
| **A/B** | comparação em que apenas um eixo muda |
| **Effective** | estado que realmente foi usado no frame |

---

## 18. Perguntas frequentes

### “UseGI” liga toda a iluminação global?

Não. Ele liga o **volume DDGI**. Primário, fallback, ReSTIR GI e cache possuem
controles e capacidades próprios.

### SHaRC substituiu o DDGI?

Não. SHaRC é o terminador primário dos hits elegíveis quando ReSTIR GI está
ativo. DDGI continua como fallback, fonte volumétrica e auxiliar de superfície.

### Por que a opção ReSTIR + SHaRC pode mostrar DDGI?

Porque o pedido degrada para DDGI quando o passe ReSTIR GI não está ligado e
pronto. Consulte o estado efetivo no manifesto ou na telemetria.

### ReGIR cria iluminação?

Ele melhora a seleção de luzes locais em hits secundários. Com ReGIR desligado,
o caminho de referência ainda avalia as luzes.

### Por que a imagem demora a responder a uma mudança?

DDGI, ReSTIR, cache, reflexões e denoisers acumulam histórico. Verifique se o
setter invalidou o domínio correto e espere a reconvergência.

### Posso comparar duas screenshots tiradas manualmente?

Para inspeção rápida, sim. Para concluir qualidade ou performance, use a
captura determinística com bookmark, preset, warmup e manifesto.

### Uma espera DDGI async pequena significa que o DDGI é grátis?

Não. Significa apenas que pouco trabalho restou no ponto da espera. A fila
compute ainda pode competir com raster por recursos da GPU.

### Por que um debug target fica preto?

Pode ser um estado correto: passe desligado, cache em warmup, fallback preto,
recurso neutro ou política efetiva degradada. Verifique telemetria e manifesto
antes de assumir falha.

---

## Próximas leituras

1. [Arquitetura geral](ARCHITECTURE.md)
2. [Protocolo de captura](CAPTURE-PROTOCOL.md)
3. [Plano SHaRC como GI primário](SHARC-PRIMARY-GI-PLAN.md)
4. [Auditoria do DDGI](DDGI-AUDIT.md)
5. [Plano de mesh lights](MESH-LIGHTS-PLAN.md)
6. [Motion vectors temporais](TEMPORAL_MOTION_VECTORS.md)

Para começar a codar, leia primeiro
[`IndirectPolicy.h`](../Engine/Include/Smile/Graphics/IndirectPolicy.h) e
[`HistoryDomain.h`](../Engine/Include/Smile/Graphics/HistoryDomain.h). Eles
documentam as duas regras que mais evitam bugs neste sistema: distinguir
**pedido de estado efetivo** e invalidar o **histórico certo**.
