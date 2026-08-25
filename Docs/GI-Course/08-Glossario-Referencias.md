# Capítulo 8 — Glossário e Referências

> [Índice](00-Indice.md) · Anterior: [Capítulo 7 — Denoising, Histórico e Debug](07-Denoising-Temporal-Debug.md)

Último capítulo do curso: um glossário único com todo termo técnico usado ao longo dos
capítulos 1–7, seguido dos papers e SDKs que a SmileEngine implementa ou referencia
diretamente no código.

## 8.1 Glossário

**Acumulador / acumulação temporal** — qualquer estrutura que combina o valor deste frame
com o(s) valor(es) de frames anteriores, em vez de recalcular do zero. Troca ruído
instantâneo por estabilidade ao longo do tempo, ao custo de reagir mais devagar a mudanças.
(Capítulos 3, 5, 7)

**Atlas** — uma textura grande que empacota muitos "retalhos" pequenos lado a lado (por
exemplo, um tile por sonda de DDGI). (Capítulo 3)

**BLAS / TLAS** (*Bottom/Top-Level Acceleration Structure*) — a hierarquia em dois níveis
que permite a uma GPU responder rapidamente "o que existe nesta direção?" sem testar
triângulo por triângulo. BLAS = por malha; TLAS = a lista de instâncias da cena, cada uma
apontando para uma BLAS. (Capítulo 2)

**Bias (de superfície)** — deslocamento pequeno aplicado à posição de consulta ou de origem
de um raio, para evitar auto-interseção/auto-sombra causada por imprecisão numérica.
(Capítulo 3)

**Bindless** — acessar recursos (texturas, buffers) da GPU por índice inteiro dentro de um
heap gigante compartilhado, em vez de "amarrar" um recurso específico a um slot fixo antes
de desenhar. É o que permite a um shader de compute genérico ler a textura certa depois de
um `TraceRayInline`, sem saber de antemão qual objeto o raio vai encontrar. (Capítulo 2)

**Boiling / filtro de fervura** — artefato do ReSTIR em que um raio de sorte encontra uma
amostra desproporcionalmente brilhante e o peso resultante se espalha pelo reuso espacial,
criando um pixel (ou mancha) branco piscando. `BoilingStrength` amortece esse pico.
(Capítulo 5)

**Bounce (quique)** — cada vez que um raio de luz encontra uma superfície e é refletido para
outra direção. Luz direta = zero quiques desde a fonte; luz indireta = um ou mais.
(Capítulo 1)

**BRDF** (*Bidirectional Reflectance Distribution Function*) — a função matemática que
descreve como um material específico reflete luz (o quanto é difuso, o quanto é especular,
etc.). (Capítulos 2, 5)

**Cascata** — no DDGI, uma de várias grades de sondas sobrepostas em resoluções diferentes;
a mais grossa cobre a cena inteira, as mais finas seguem a câmera com maior densidade.
(Capítulo 3)

**Cell / célula** — no SHaRC, a unidade de armazenamento da tabela de hash: uma região do
espaço (definida por posição quantizada + octante de normal + nível de LOD) que guarda uma
radiância acumulada. (Capítulo 4)

**Checksum** — no SHaRC, um segundo valor de hash guardado junto da entrada, usado para
confirmar que a célula encontrada é realmente a procurada e não uma colisão acidental de
hash. (Capítulo 4)

**Chebyshev, teste de** — o teste estatístico (originado em *variance shadow maps*) que o
DDGI usa para saber se uma sonda "enxerga" um ponto sem obstrução, comparando a distância
pedida contra a média e a variância da distância registrada naquela direção. É o mecanismo
que evita vazamento de luz através de paredes finas. (Capítulo 3)

**Compute assíncrono** — trabalho de GPU disparado numa fila separada da fila principal
("direta"), rodando em paralelo com ela. O update do DDGI roda assim, "escondido" atrás de
outros passes que já iam acontecer. (Capítulo 2)

**Deferred shading** — técnica de rasterização em que a cena é primeiro desenhada num
conjunto de texturas intermediárias (G-buffer: cor, normal, rugosidade...) e a iluminação é
calculada depois, num passe separado, lendo essas texturas. (Capítulos 1, 2)

**Denoiser / denoising** — filtro que reduz o ruído de uma imagem gerada por amostragem
estocástica, tipicamente combinando informação temporal (frames anteriores) e espacial
(pixels vizinhos). Na Smile: NRD. (Capítulo 7)

**Dirty flag** — uma marcação de "isto mudou e precisa ser reprocessado", frequentemente
"coalescida" (acumulada e consumida uma vez só por frame) para evitar trabalho repetido
quando o mesmo parâmetro muda várias vezes entre dois frames. (Capítulo 7)

**Fallback** — o sistema que responde quando o sistema primário não consegue. No contexto de
GI da Smile: o DDGI é o fallback do SHaRC. (Capítulos 1, 4, 6)

**Fibonacci, espiral/esfera de** — método para distribuir N pontos numa esfera de forma quase
uniforme, sem aglomerar nos polos, usando a razão áurea. O DDGI usa isso para direcionar os
64 raios de cada sonda. (Capítulo 3)

**G-buffer** — o conjunto de texturas intermediárias produzido pelo passe de geometria em
deferred shading (cor base, normal, rugosidade, metalicidade, profundidade, velocidade).
(Capítulos 1, 2)

**GI (Iluminação Global / Global Illumination)** — o conjunto de técnicas para calcular luz
indireta (Capítulo 1). Termo genérico da indústria, não específico da Smile.

**Hash grid / tabela de hash** — estrutura de dados que mapeia uma chave (aqui, posição +
normal + LOD) para um slot através de uma função de hash, permitindo consulta em tempo
aproximadamente constante sem precisar de uma grade fixa pré-alocada para todo o espaço
possível. Base do SHaRC. (Capítulo 4)

**Histerese** — no contexto de acumulação temporal, o quanto o valor novo pesa contra o
valor antigo numa média móvel: `novo = lerp(antigo, este_frame, 1 − histerese)`. Histerese
alta = mudança lenta e estável; baixa = reage rápido mas com mais ruído. (Capítulos 3, 5)

**Instância** — uma ocorrência específica de uma malha na cena (posição, rotação, escala,
material) — múltiplas instâncias podem compartilhar a mesma malha-base (BLAS).
(Capítulo 2)

**`InstanceGeo`** — o snapshot bindless, indexado por `InstanceID`, que descreve o que cada
instância da TLAS "é" (buffers de vértice/índice, texturas de material) para qualquer raio
que a atinja. (Capítulo 2)

**Invalidação de histórico** — o ato de zerar (ou reduzir a confiança de) um acumulador
temporal porque o parâmetro que ele descreve mudou. Ver `HistoryDomain.h` (Capítulo 7).

**Irradiância** — a medida de "quanta luz chega a um ponto, vinda de todas as direções
relevantes", integrada sobre o hemisfério. É o que cada sonda de DDGI guarda.
(Capítulo 3)

**Jacobiano (de reconexão)** — o fator de correção matemática aplicado ao reaproveitar, no
reuso espacial do ReSTIR, uma amostra calculada para um ponto de vista ligeiramente
diferente do atual. (Capítulo 5)

**LOD** (*Level of Detail*) — reduzir a resolução/precisão de uma representação conforme a
distância aumenta. No SHaRC, células dobram de tamanho a cada nível de distância da câmera.
(Capítulo 4)

**Luz direta** — luz que sai de uma fonte e atinge uma superfície sem quicar em nada no
caminho. (Capítulo 1)

**Luz indireta** — luz que chega a uma superfície depois de pelo menos um quique em outra
superfície. Sinônimo prático de "GI" no contexto deste curso. (Capítulo 1)

**Máscara de raio** — um filtro de categoria (opaco, alpha-test, translúcido) que determina
quais instâncias um raio específico "enxerga" ao ser traçado. (Capítulo 2)

**Octaédrico, mapeamento** — técnica de projetar direções de uma esfera 3D sobre um quadrado
2D (via um octaedro intermediário), usada para guardar "toda direção" numa textura pequena.
(Capítulos 3, 5)

**Path tracing** — a técnica de renderização fisicamente exata: traçar caminhos completos de
luz, quique a quique, e somar a contribuição. Cara demais para tempo real em qualidade de
filme, mas usada em forma "leve" dentro do updater do SHaRC. (Capítulos 1, 4)

**Persistente (histórico persistente)** — informação que sobrevive de um frame para o
seguinte, em oposição a algo recalculado do zero a cada frame. (Capítulo 7)

**pHat / função-alvo** — no ReSTIR, a função (aqui: luminância × cosseno) que dá "nota" a
uma amostra candidata, usada para decidir a probabilidade dela vencer a reamostragem.
(Capítulo 5)

**Probe / sonda** — no DDGI, um dos pontos fixos da grade 3D que amostra e guarda a
irradiância local da cena. (Capítulo 3)

**Rasterização** — a técnica clássica e rápida de desenhar triângulos projetando-os
diretamente na tela; responde "que triângulo cai neste pixel", mas não sabe responder
perguntas sobre pontos/direções arbitrários fora da tela. (Capítulos 1, 2)

**RayQuery inline** — a forma de fazer ray tracing do DirectX Raytracing (DXR) 1.1 em que um
raio é traçado com uma chamada de função dentro de um compute shader comum, sem trocar de
shader especializado. É o único modelo usado pela Smile. (Capítulo 2)

**Relocação** — no DDGI, o processo de deslocar gradualmente uma sonda para fora de
geometria sólida onde ela tenha "nascido" enterrada. (Capítulo 3)

**Reservoir** — a estrutura central do ReSTIR: guarda "o melhor candidato até agora" (e
metadados suficientes para calcular um peso não-enviesado), sem precisar guardar todos os
candidatos vistos. (Capítulo 5)

**ReSTIR** (*Reservoir-based Spatio-Temporal Importance Resampling*) — a família de técnicas
de reamostragem por reservatório, publicada por Bitterli et al. (2020) e estendida para GI
por Ouyang et al. (2021). Na Smile: ReSTIR DI, ReSTIR GI, e o ReGIR relacionado.
(Capítulo 5)

**Reuso espacial** — no ReSTIR, combinar o reservoir do pixel atual com o de pixels vizinhos
no mesmo frame. (Capítulo 5)

**Reuso temporal** — no ReSTIR, combinar o reservoir do pixel atual com o do mesmo pixel
(reprojetado) no frame anterior. (Capítulo 5)

**SHaRC** (*Spatial Hashing Radiance Cache*) — a técnica de cache de radiância em espaço de
mundo via tabela de hash, publicada pela NVIDIA. Na Smile: `FRadianceCache`. (Capítulo 4)

**Shadow ray** — um raio disparado para testar apenas "existe algo entre este ponto e a
fonte de luz?", sem precisar saber o quê ou a que distância exata. (Capítulo 2)

**Terminador** (do raio) — o sistema que responde "qual é a radiância aqui" no fim de um
caminho de raio secundário, dentro do funil `ShadeSurfaceHit`. É o termo interno da Smile
para "quem decide o valor final naquele ponto". (Capítulos 2, 6)

**Vazamento de luz (*light leaking*)** — artefato clássico de técnicas de sondas/grades em
que luz "atravessa" indevidamente uma parede fina ou outra separação fina entre dois
espaços que deveriam estar opticamente isolados. (Capítulo 3)

**Warmup / aquecimento** — o período, depois de uma tabela/cache ser criada ou resetada, em
que ela ainda não acumulou amostras suficientes para ser consultada com confiança.
(Capítulos 4, 7)

**WRS** (*Weighted Reservoir Sampling*) — o algoritmo matemático de base do ReSTIR: escolher
um "vencedor" entre um fluxo de candidatos com peso variável, sem guardar a lista inteira,
de forma estatisticamente correta. (Capítulo 5)

## 8.2 Papers e referências citados diretamente no código

Esta lista reúne as fontes acadêmicas e técnicas que os comentários do código-fonte da
Smile citam nominalmente — vale a pena consultá-las para ir além do que este curso cobriu.

| Referência | Onde é usada na Smile |
|---|---|
| Majercik et al., **"Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields"**, JCGT 2019 | DDGI — o algoritmo base das sondas (Capítulo 3) |
| **SHaRC** (SDK/paper de referência da NVIDIA) | Radiance cache, incluindo o Listing 7.6 sobre octantes de normal (Capítulo 4) |
| Gautron, **"Real-Time Global Illumination Using Spatial Hashing"**, 2020 | Fórmula de nível de LOD por distância no SHaRC (Capítulo 4) |
| Sousa, 2025 (referência interna do código) | LOD e escala de células do SHaRC (Capítulo 4) |
| Bitterli et al., **"Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing"** (ReSTIR original), SIGGRAPH 2020 | Base matemática do ReSTIR DI (Capítulo 5) |
| Ouyang et al., **"ReSTIR GI: Path Resampling for Real-Time Path Tracing"**, 2021 | ReSTIR GI — o final gather difuso por pixel (Capítulo 5) |
| *Ray Tracing Gems II*, capítulo 23 | ReGIR — grade de reservoirs para luzes locais em hits secundários (Capítulo 5) |
| *Ray Tracing Gems II*, capítulo 25 | O motion vector temporal dual (`FTemporalMotionVectors`), citado no Capítulo 5 como base da reconstrução do ponto visível do ReSTIR |
| RTXGI SDK (NVIDIA) | Referência de implementação para vários detalhes finos do DDGI (pisos numéricos, convenções) — comparado explicitamente na auditoria interna |
| Flax Engine (código-fonte aberto) | Comparação de referência para vários detalhes de implementação do DDGI (citada linha a linha em `Docs/DDGI-AUDIT.md`) |
| idTech 8 (*DOOM Eternal*) | Referência para o `BaseCellSize = 0.25m` do SHaRC e para o `UpdateFraction = 0.04` |
| Cyberpunk 2077: Overdrive (CD Projekt Red) | Referência de escala (capacidade de tabela de hash) e de fração de atualização do radiance cache |

## 8.3 Documentação interna relevante (para quem quer ir mais fundo no código)

Este curso é uma introdução; a documentação técnica original, mais densa e voltada para
quem já vai editar o código, continua sendo a fonte primária:

| Documento | Conteúdo |
|---|---|
| `Docs/ARCHITECTURE.md` | Visão geral da engine inteira — build, módulos, backend D3D12, ordem do frame |
| `Docs/RENDERER.md` | Mapa físico do `Renderer`, fronteiras de ownership, invariantes essenciais |
| `Docs/DDGI-AUDIT.md` | A auditoria completa do DDGI: cada bug encontrado, cada correção, cada medição — inclusive as fases de cascata e scrolling toroidal |
| `Docs/SHARC-PRIMARY-GI-PLAN.md` | O plano de implementação fase a fase do SHaRC, da telemetria à promoção a fonte primária |
| `Docs/CAPTURE-PROTOCOL.md` | O protocolo completo de captura determinística — a régua de medição por trás de todos os números citados neste curso |
| `Docs/KNOBS-AUDIT.md` | Auditoria de parâmetros ajustáveis e sua invalidação de histórico |
| `Docs/MESH-LIGHTS-PLAN.md` | Luzes emissivas extraídas de malhas (fora do escopo deste curso) |
| `Docs/TEMPORAL_MOTION_VECTORS.md` | O sistema de motion vectors duais que sustenta o reuso temporal do ReSTIR |

## 8.4 Fim do curso — e daqui para onde?

Você chegou ao fim dos oito capítulos. O caminho recomendado, se quiser aprofundar de
verdade, é abrir os arquivos citados ao longo do curso lado a lado com o código: comece por
`Engine/Include/Smile/Graphics/GI/IndirectPolicy.h` (Capítulo 6) — é curto, comentado
exaustivamente, e amarra tudo o que os capítulos 3–5 explicaram separadamente. Depois,
`Shaders/GI/HitShading.hlsli` (Capítulo 2) mostra, em código de verdade, o funil por onde
toda essa teoria efetivamente vira pixels na tela.

Bom estudo — e bem-vindo ao código da Smile.

[Voltar ao Índice](00-Indice.md)
