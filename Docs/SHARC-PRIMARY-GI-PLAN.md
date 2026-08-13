# SHaRC/WRC como GI primário, DDGI como fallback

## ESTADO — 2026-08-13

**A régua está pronta e calibrada.** O `Docs/CAPTURE-PROTOCOL.md` é a referência; aqui fica só o
que muda o plano.

Capturador completo: domínio `DeterministicCapture` (o universo dos alvos, inclusive os caches de
mundo), `FFrameCapture` com aquecimento em frames **renderizados**, PNG pós-tonemap/pré-overlays,
manifesto derivado do estado **efetivo**, presets científico/gameplay, disparo pela UI ao lado dos
bookmarks, e o commit da build carimbado a cada *build*. Commits `49b2e88`, `1a16ba7`, `4ef2cf4`,
`3872f97`.

**N = 128, calibrado** por sweep de 32/64/128/256. A ocupação do cache termina em 64, mas a
repetibilidade da imagem só estabiliza em 128 (50,6 → 54,4 → 59,3 dB), e 256 rende 0,03 dB pelo
dobro da espera. Escolher pela ocupação teria dado metade do necessário — que é precisamente o erro
que o plano previa ao exigir o delta do sinal final na lista de sinais.

Três achados que a régua produziu antes de medir qualquer estimador, e que valem para o resto da
série:

- `SunDir` nascia **não normalizado**, e a restauração de estado o convertia: o manifesto divergia
  entre duas capturas idênticas. Corrigido na origem.
- Dois acumuladores reais (`SunShafts`, FFT do oceano) nunca tinham entrado no `EHistoryTarget`, e
  "reset de tudo" era falso.
- A **instrumentação do cache não é neutra**: os atômicos mudam o escalonamento das waves e, com
  ele, quais threads vencem as inserções (73.218 × 73.195 células, 48 dB entre os regimes). Ela é
  parte da configuração — entra no manifesto e no nome do arquivo, e alterná-la cancela a captura.

**O que falta da Fase 0 são as quatro baselines**, agora com regra de operação definida:
instrumentação **desligada** (elas são comparação de imagem), TOD com hora fixada, build limpa. As
quatro configurações da matriz já são alternáveis pela UI. Depois delas, o commit #4.

## ESTADO — 2026-08-12

Bloco de retomada. Tudo abaixo já foi decidido e verificado no código; não re-derivar.

### O que já entrou

| Commit | Fase | O quê |
|---|---|---|
| `1a52ce9` | 1a | `InstanceGeo` sai do `FDDGI` → `FRaytracingScene`. Mais 3 correções de lifetime (dreno das duas filas no `BuildRaytracingScene`; `ComputeQueue.WaitIdle` nos dois `RefreshInstanceGeo`; `FReSTIRDI::RefreshMeshLightDescriptors`, que consertou tela branca ao religar o DI depois de duplicar objetos). |
| `14571d7` | 1b | Early return `if (!DDGI.IsReady()) return;` removido. `FGIFallbackBindings` + recursos neutros 1×1 zerados. `WantNrdIndirect()` solto do volume. Gate no shader via `FGIHitSampling::FallbackAvailable`. |
| `a67eadd` | 1b | Fallback escolhido por **existência** do volume (`DDGI.IsReady()`), não por `UseGI`. |
| `3361762` | régua | Bookmarks de câmera, 4 slots, sidecar `<cena>.cameras.json`. |
| `05a88f7` | régua | `TemporalSampleIndex` separado do `FrameIndex`; incremento movido para o fim do frame. |

**Fase 1 está fechada.** Fase 0 nunca teve capturas registradas — é o que a régua existe para
resolver, e por isso ela entrou antes do commit #4 em vez de depois.

### Conclusões que não devem ser re-derivadas

- **Fase 0 já estava quase toda em código.** As quatro configurações de A/B são alternáveis pela UI
  hoje, e a flag "DDGI no hit secundário" que o plano pede para acrescentar já existe:
  `FGIHitSampling::TerminatorOff` → bit 2 do `SkipModePacked()` → `termOff` no `HitShading.hlsli`,
  exposta como `giMeasureTerminatorOff`. O que falta da Fase 0 é **medição**, não código.
- **`HistoryDomain::CameraCut` NÃO derruba os caches de mundo** (`DDGIAtlas`, `ReGIR`,
  `RadianceCache`), de propósito. Correto para navegação, errado para captura — daí o domínio
  `DeterministicCapture` pendente.
- **O aquecimento é de centenas de frames**, não dezenas: histerese 0,99 no DDGI (~199 updates para
  o atlas de distância) e teto de 64 amostras no radiance cache.
- **Existência × habilitação.** O que é lido no setup fica *latched* na tabela de descritores; o que
  é lido por frame não. `UseGI` muda sem provocar setup, então não pode participar de escolha de
  descritor. Critério para qualquer campo novo no `FGIFallbackBindings`.
- **`BuildRaytracingScene()` tem de ser seguido de `SetupGIForScene()`** — a tabela de trace do DDGI
  carrega uma *cópia* do descritor do snapshot e não acompanha a realocação sozinha.
- **Preset científico:** desligar o upscaler **ativa** o TAA (`TAAActive = UseTAA && !UpscaleActive`),
  trocando jitter de FSR por Halton. Tem de zerar `UseTAA` também.
- `Time.z` do cbuffer carrega o `FrameIndex` e **nenhum shader o lê**. Payload morto, removível.

### O que falta, em ordem

1. ~~**Capturador**~~ — feito, exceto a **calibração do N** (instrumentação de convergência, teto
   de 512). Ver o bloco de estado no topo e o `Docs/CAPTURE-PROTOCOL.md`.
2. **Commit #4** — `gi: split hit shading from indirect fallback policy`. Quebra o
   `ShadeSurfaceHit`, introduz `FPathState` e `FRCQueryResult`. É o primeiro teste real da régua:
   promete não mudar imagem, então serve para validar o capturador.
3. **Fase 3 em diante** — o path tracer esparso, onde o estimador muda de verdade.

### Ressalva sobre comparação com builds antigas

O `05a88f7` moveu o incremento dos contadores para o fim do frame, o que desloca algumas sementes
em −1. Imagem contra builds anteriores **não é bit a bit**; o critério é "o ruído mudou, a energia
não".

---

## Objetivo

Transformar o `FRadianceCache` atual em um cache world-space de radiância no estilo SHaRC, alimentado por um path tracer esparso e usado como terminador primário do transporte indireto. O `FReSTIRGI` continua responsável pelo reúso espaço-temporal do ponto secundário. O DDGI deixa de definir o sinal principal e passa a ser consultado somente quando o cache não consegue responder com confiança, além de continuar atendendo consumidores que realmente precisam de irradiância volumétrica, como fog.

O estado final pretendido é:

```text
ReSTIR DI ------------------------------------------------------> direta

G-buffer -> ReSTIR GI / indirect path sample -> SHaRC/WRC -----> indireta
                                        miss / cache frio |
                                                          +----> DDGI fallback
                                                          +----> ambiente/zero se DDGI off

Sparse cache-update path tracer -> Accum -> Resolve -> SHaRC/WRC do próximo frame
```

Esta implementação deve evoluir o que já existe. Não importar uma implementação inteira de SHaRC nem substituir imediatamente o ReSTIR GI próprio pela RTXDI 3.0. O escopo padrão tem dez fases, de 0 a 9, e termina com o `FReSTIRGI` atual usando SHaRC/WRC como fonte primária e DDGI como fallback. ReSTIR PT é um épico futuro, opcional e dependente de autorização separada.

## Decisões de arquitetura

1. `FRadianceCache` continua sendo o dono da tabela, resolve, debug e telemetria. Ele ganha um produtor explícito de paths; não nasce um segundo cache concorrente.
2. O cache guarda radiância de saída world-space, não irradiância. Ele permanece não direcional. Superfícies especulares só podem consultá-lo quando o cone do raio cobrir ao menos um voxel; contribuições estreitas não devem alimentá-lo.
3. A atualização deixa de acontecer incidentalmente em todo `ShadeSurfaceHit`. Somente o passe dedicado de update escreve radiância no cache. Os traces de render consultam, mas não treinam o cache.
4. O passe de update nunca usa DDGI como fonte normal de radiância. Caso contrário, o cache apenas esconderia um sinal cuja origem continuaria sendo DDGI. Um modo de bootstrap com DDGI pode existir apenas como knob de diagnóstico e deve nascer desligado.
5. `Accum` recebe o frame atual; `Resolved` é somente leitura para os traces do mesmo frame. O resolve continua no fim e publica o resultado para o frame seguinte. Essa separação é a barreira contra auto-realimentação.
6. O hash de 32 bits + checksum + linear probing atual é mantido inicialmente. Ele entrega o comportamento necessário sem exigir `InterlockedCompareExchange64` e custa 36 MiB em `2^20`, contra aproximadamente 167 MB da configuração do Cyberpunk.
7. DDGI é fallback por hit e por capacidade do hardware, não outro “look” artístico. Materiais, luzes, intensidade e exposição são comuns aos dois caminhos.
8. ReSTIR DI e os dois domínios de denoising permanecem separados. Direta continua em `NrdDirect`; GI/reflexos continuam no `Nrd` indireto.
9. RTXDI ReSTIR PT fica fora do caminho crítico e do critério de conclusão. O cache deve funcionar com o `FReSTIRGI` atual; o apêndice de ReSTIR PT só é executado se houver autorização e hardware-alvo compatível.

## Estado atual que não pode ser ignorado

- `Renderer::SetupForScene` já monta `RadianceCache` independentemente da AABB.
- O setup de resize ainda contém `if (!DDGI.IsReady()) return;` antes de reflexões, ReSTIR GI e NRD indireto em `Engine/Source/Graphics/Renderer.cpp`.
- `FReSTIRGI::SetupForResize` exige atlas de irradiância, atlas de distância e `ProbeData` do DDGI.
- O snapshot bindless `InstanceGeo` ainda é exposto por `FDDGI::InstanceSRV()`, apesar de ser dado geral da cena de RT.
- `ShadeSurfaceHit` consulta o cache cedo, mas, em miss, calcula direto + emissivo + DDGI e grava esse resultado de volta com `RC_Update`. Portanto, hoje o cache aprende principalmente um sinal derivado do DDGI.
- O cache atual já possui LOD por distância, normal por octante, proteção por comprimento do segmento, gate de cone especular, média temporal, refresh, tombstones, evicção, visualizador e readback de métricas.
- O `FReSTIRGI` já possui trace inicial, reuso temporal, reuso espacial, Jacobiano, limite de idade configurável, boiling filter, pack para NRD e histórico de superfície confiável.
- O RTXDI local é 3.0 e contém ReSTIR PT, mas o SmileEngine não liga o runtime; atualmente apenas replica/adapta partes dos algoritmos nos shaders próprios.
- O worktree contém alterações locais relevantes em DDGI, `HitShading`, `ReSTIRGITrace`, fog, editor e outros arquivos. O implementador deve preservar tudo, nunca usar reset/checkout destrutivo e revisar o diff existente antes de tocar em arquivos sobrepostos.

## Conclusões anteriores que este plano deve preservar

Esta seção existe para que uma compactação de contexto ou troca de implementador não apague decisões já tomadas durante a leitura das três referências.

### idTech8 / DOOM: WRC eficiente, mas não SHaRC de paths

- O WRC da idTech8 é um hash intermediário de shading em um pipeline híbrido, não o mesmo estimator do SHaRC do Cyberpunk.
- As probes traçam visibilidade e produzem hit packets compactos de 128 bits. O sistema deduplica/hash os hits antes do shading, forma listas de células ativas e sombreia por indirect dispatch/material; a apresentação cita aproximadamente 20 mil células sombreadas por frame.
- O WRC usa células-base de 25 cm com LOD, orçamento de aproximadamente 14 MiB e reaproveitamento temporal.
- O final gather não volta a sombrear o material. A hierarquia é, conceitualmente, screen-space, depois WRC e, em miss, irradiance volumes.
- A principal lição para a Smile não é copiar o estimator da idTech, mas copiar scheduling, compactação, deduplicação antes do shading, work lists por material e disciplina de memória.

### Assassin's Creed Shadows: DDGI excepcional, porém adequado como fallback

- O sistema usa cinco cascatas de `16 x 16 x 8`, total de 10.240 probes e cobertura de cerca de 512 m, com scroll toroidal e spacing de 2 a 32 m.
- Cada probe mantém um G-buffer persistente. Traçar/atualizar visibilidade e relight são operações separadas; somente uma fração das probes recebe trace completo por frame, enquanto o relight é mais frequente.
- Metadados de luminância permitem reagir rapidamente a Time of Day mesmo em probes não selecionadas para trace completo.
- As probes publicam radiância direcional e irradiância difusa separadamente. O multi-bounce lê a irradiância do frame anterior.
- O final gather difuso usa primeiro screen trace e depois BVH. Isso reforça que o caminho mais barato deve ser tentado antes de um trace completo quando a validação for confiável.
- A própria apresentação registra os limites de distribuição uniforme e leaking e aponta importance sampling, surface probes e estruturas esparsas como direções futuras. Por isso, aqui o DDGI permanece como cobertura ampla/fog/cache frio, não como sinal primário de superfície.

### Cyberpunk 2077: referência principal para a arquitetura escolhida

- ReSTIR GI e SHaRC são complementares. ReSTIR GI reutiliza o ponto/path secundário; SHaRC fornece radiância world-space para aumentar a qualidade dos bounces e terminar caminhos.
- O update do SHaRC é independente: cerca de 4% dos pixels, até quatro bounces, contra o caminho de render mais curto. Ele pode terminar consultando o cache do frame anterior e faz backpropagation.
- A consulta usa posição, normal, LOD logarítmico e regras de comprimento do segmento/tamanho do voxel e cone especular.
- A configuração publicada usa `2^22` entradas e aproximadamente 167 MB. A Smile deliberadamente começa em `2^20` e aproximadamente 36 MiB.
- O ReSTIR GI prático do jogo limita o histórico a oito frames. Para specular, reduz o raio espacial com roughness e desliga o spatial abaixo de `0.1`.
- Direta e indireta permanecem em domínios de denoising separados porque seus perfis de variância são radicalmente diferentes.
- O jogo combina a escolha de lobo difuso/especular em um trace inicial e corrige o PDF da mistura. Isso pode virar uma otimização futura do pipeline atual sem exigir ReSTIR PT completo, mas não é requisito para colocar SHaRC como primário.

## Regras para o implementador

- Cada fase termina em estado compilável e executável.
- Não misturar refactor estrutural, mudança de estimator e tuning visual no mesmo commit.
- Não apagar o caminho DDGI existente enquanto os gates finais não tiverem passado.
- Novos recursos de GPU entram no `VramTracker`/categoria GI e recebem nome de debug.
- Todo recurso lido por descriptor table deve receber descriptor válido mesmo quando o fallback estiver desligado; o shader é quem fecha a leitura por flag.
- Mudança de layout de cbuffer exige `static_assert` de tamanho/offset no C++ e comentário de contrato no HLSL.
- Mudanças que alterem o significado do histórico invalidam os domínios corretos: cache, ReSTIR GI, NRD/RR e TAA conforme o consumidor.
- Antes de otimizar, capturar baseline de imagem, custo, VRAM e métricas. “Parece melhor” não fecha fase.
- Não integrar ReSTIR PT durante o escopo padrão. Duas mudanças estatísticas grandes simultâneas tornam qualquer regressão impossível de localizar; o apêndice futuro exige autorização própria.

## Fase 0 — Congelar baseline e criar a matriz A/B

### Objetivo

Documentar o comportamento atual e criar modos que permitam provar de onde vem cada contribuição.

### Trabalho

- Registrar o estado inicial do worktree com `git status --short` e salvar o diff dos arquivos que a fase tocar. Não reverter alterações do usuário.
- Usar uma câmera determinística e capturar ao menos:
  - uma área predominantemente indireta;
  - uma parede fina com lados claro/escuro;
  - um emissivo forte;
  - uma sequência de roughness de 0 a 1;
  - uma cena com sol/Time of Day variando;
  - um teleport de câmera;
  - objeto e luz em movimento.
- Registrar GPU timings do frame e, separadamente:
  - ReSTIR GI temporal + trace;
  - ReSTIR GI espacial + resolve;
  - radiance cache resolve;
  - DDGI trace/update/relocate/classify;
  - NRD indireto;
  - ReSTIR DI e NRD direto como controles.
- Registrar VRAM de DDGI, ReSTIR GI, NRD, reflexos e radiance cache.
- Capturar quatro baselines:
  1. DDGI principal, cache desligado;
  2. cache atual atualizando, query desligada;
  3. cache atual atualizando e consultando;
  4. ReSTIR GI desligado, DDGI puro.
- Acrescentar, se não houver equivalente, uma flag de diagnóstico “DDGI no hit secundário” que desliga somente o gather dentro de `ShadeSurfaceHit`, sem desligar o DDGI do deferred/fog. O bit já parcialmente existe em `SkipModePacked`; torná-lo um modo explícito e nomeado.

### Arquivos prováveis

- `Engine/Include/Smile/Graphics/RenderSettings.h`
- `Engine/Source/Graphics/RenderSettings.cpp`
- `Editor/Source/RenderSettingsBridge.cpp`
- `Editor/Qml/SettingsWindow.qml`
- `Engine/Source/Graphics/Renderer.cpp`
- `Shaders/GI/HitShading.hlsli`

### Gate de saída

- As quatro configurações podem ser alternadas sem restart.
- O profiler e a UI não apresentam métricas velhas de um passe desligado.
- Existe uma captura reproduzível antes de qualquer mudança de estimator.
- Build de shaders e do editor passa.

## Fase 1 — Desacoplar a infraestrutura de RT do DDGI

### Objetivo

Permitir que ReSTIR GI, reflexões, ReGIR e temporal motion inicializem com TLAS e materiais válidos mesmo quando o volume DDGI não existir.

### Trabalho

- Mover a propriedade/publicação do snapshot `InstanceGeo` para `FRaytracingScene` ou para uma pequena estrutura comum `FRTSceneBindings`.
- `FDDGI`, `FReSTIRGI`, `FReSTIRDI`, `FReflections`, `FTemporalMotionVectors`, mesh lights e o futuro cache updater passam a receber o mesmo `InstanceSRV` comum.
- Separar os pré-requisitos do bloco de resize do renderer:
  - reflexões precisam de TLAS, instâncias, G-buffer, depth, velocity, ambiente e recursos de fallback válidos;
  - ReSTIR GI precisa de TLAS, instâncias, G-buffer, depth, velocity, ambiente e fallback opcional;
  - NRD indireto depende de ReSTIR GI/reflexos e do modo de denoiser, não de `DDGI.IsReady()`;
  - somente o volume DDGI e seus debug passes dependem de `DDGI.IsReady()`.
- Remover o early return global após `SetupNrdDirect`. Substituí-lo por setup condicional de cada passe.
- Alterar `WantNrdIndirect()` para depender do produtor indireto ativo, não do DDGI.
- Criar bindings neutros válidos para fallback ausente, preferencialmente recursos 1x1/1-elemento zerados pertencentes ao renderer. Não copiar `kInvalidSlot` para descriptor tables.
- Introduzir um contrato explícito, por exemplo:

```cpp
struct FGIFallbackBindings {
    u32 IrradianceAtlasSRV;
    u32 DistanceAtlasSRV;
    u32 ProbeDataSRV;
    FDDGICascadeConstants Cascades;
    bool Available;
};
```

- O nome pode mudar, mas o conceito deve impedir que a classe `FReSTIRGI` volte a depender diretamente de `FDDGI`.

### Arquivos prováveis

- `Engine/Include/Smile/Graphics/RaytracingScene.h`
- `Engine/Source/Graphics/RaytracingScene.cpp`
- `Engine/Include/Smile/Graphics/DDGI.h`
- `Engine/Source/Graphics/DDGI.cpp`
- `Engine/Include/Smile/Graphics/ReSTIRGI.h`
- `Engine/Source/Graphics/ReSTIRGI.cpp`
- `Engine/Include/Smile/Graphics/ReSTIRDI.h`
- `Engine/Source/Graphics/ReSTIRDI.cpp`
- `Engine/Include/Smile/Graphics/Reflections.h`
- `Engine/Source/Graphics/Reflections.cpp`
- `Engine/Source/Graphics/Renderer.cpp`

### Gate de saída

- Com DDGI desabilitado/não pronto, ReSTIR DI continua funcionando.
- ReSTIR GI e reflexões chegam a `IsReady()` com descriptors válidos.
- ReSTIR GI produz direto no hit + emissivo + ambiente/zero no terminal, sem access violation e sem ler atlas DDGI inválido.
- D3D12 debug layer não reporta descriptor, state ou lifetime errors.
- A imagem com DDGI habilitado permanece equivalente ao baseline; esta fase não muda o estimator.

## Fase 2 — Refatorar o shading de hit sem mudar a imagem

### Objetivo

Separar carregamento de superfície, iluminação local, transporte indireto e política de cache. O path tracer de update precisa reutilizar exatamente o mesmo material/shading sem herdar a decisão “sempre sample DDGI”.

### Trabalho

- Quebrar `ShadeSurfaceHit` em blocos menores, mantendo um wrapper compatível para os consumidores atuais:
  - classificação/facing e offset geométrico;
  - carregamento de material e texturas;
  - direto do sol;
  - direto de luzes locais/ReGIR e shadow rays;
  - emissivo;
  - avaliação/amostragem do BSDF;
  - fallback indireto DDGI;
  - composição da radiância de saída.
- Introduzir um pequeno `FPathState` HLSL com, no mínimo:
  - origem/direção;
  - throughput;
  - depth;
  - roughness/cone do segmento;
  - PDF e lobo escolhido;
  - flags de modo: render, cache update, replay futuro.
- A API de consulta ao cache deve devolver um resultado estruturado em vez de apenas `bool`:

```hlsl
struct FRCQueryResult {
    float3 Radiance;
    float  CellSize;
    uint   SampleCount;
    uint   Age;
    uint   Status;
};
```

- Manter as regras atuais de segmento maior que voxel e cone especular maior que voxel.
- Remover o `RC_Update` do wrapper compartilhado somente quando o passe dedicado da Fase 3 existir. Até lá, protegê-lo por uma flag de produtor legado para manter a equivalência.
- Não duplicar BRDF entre o path updater e ReSTIR GI. Funções comuns devem viver em um header como `PathShading.hlsli`/`PathTracingCommon.hlsli`.

### Arquivos prováveis

- `Shaders/GI/HitShading.hlsli`
- `Shaders/GI/RadianceCache.hlsli`
- novo `Shaders/GI/PathTracingCommon.hlsli`
- `Shaders/GI/ReSTIRGITrace.cs.hlsl`
- shaders de reflexo que incluem `HitShading.hlsli`
- `Shaders/GI/DDGITrace.cs.hlsl`

### Gate de saída

- Shader output das configurações baseline permanece visualmente e numericamente equivalente dentro do ruído temporal esperado.
- Nenhum consumidor possui uma cópia divergente de material, emissivo, shadow bias ou luz local.
- Todos os shaders que incluem o contrato compilam em SM 6.6.

## Fase 3 — Criar o path tracer esparso de atualização do cache

### Objetivo

Dar ao cache uma fonte própria de radiância multi-bounce que não dependa dos hits produzidos por DDGI, ReSTIR GI ou reflexões.

### Trabalho

- Acrescentar ao `FRadianceCache` um pipeline `RadianceCacheUpdate` e um método `RecordUpdate`.
- Criar `Shaders/GI/RadianceCacheUpdate.cs.hlsl`.
- Usar pixels de G-buffer como origens dos paths. O primeiro estágio recebe depth, normal/material, `InvViewProj`, TLAS, instâncias, sky, luzes, mesh lights/ReGIR e os buffers do cache.
- Não gravar esse updater dentro de `PrepareIndirectLighting()`: hoje essa função roda antes de `RecordGBuffer()`, portanto depth/material do frame ainda não existem no estado necessário.
- O primeiro ponto de integração correto é imediatamente após `RecordGBuffer(Ctx)` e antes de `RecordSceneLighting(Ctx)`. Preferencialmente, gravar o update antes da espera do DDGI assíncrono: ele não consome DDGI e seu trabalho na fila direta pode sobrepor o compute já em voo. Restaurar depth/G-buffer aos estados que o bloco de espera e `RecordSceneLighting` esperam.
- Como passo inicial mais simples, o update pode viver no começo de `RecordSceneLighting`, antes de `ReSTIRGI.RecordTrace`; essa alternativa sacrifica overlap, mas mantém a dependência correta.
- Selecionar aproximadamente 4% dos pixels com uma sequência progressiva determinística, por exemplo uma permutação de período 25. Evitar um sorteio independente que deixe buracos por muitos frames.
- Começar com dispatch full resolution + early-out pela máscara. Só compactar depois da correção estar comprovada.
- Traçar até quatro bounces no update, com um raio por bounce e next-event estimation para sol/luz selecionada quando aplicável.
- Escolher o lobo a partir da energia difusa/especular, com PDF da mistura. Não alimentar o cache a partir de um lobo estreito que ele não consegue representar.
- Registrar por vértice, para no máximo quatro vértices:
  - posição e normal geométrica;
  - radiância direta + emissiva local;
  - throughput/PDF para o próximo vértice;
  - elegibilidade para cache;
  - distância e roughness do segmento.
- No primeiro corte de correção, consultar `Resolved` somente no terminal do path de update. O terminal pode ser emissivo, ambiente, cache anterior ou zero. Não usar DDGI.
- Fazer backpropagation em ordem reversa:

```text
L_terminal = emissivo/sky/cache anterior/zero
L_i = direto_i + emissivo_i + throughput_i * L_(i+1)
RC_Update(pos_i, normal_i, L_i) para cada vértice elegível
```

- Se o loop for implementado inteiramente num único compute, o array local deve ter tamanho fixo e pressão de registradores medida. Se occupancy despencar, usar duas etapas com buffer compacto de paths; não otimizar por suposição.
- Nomear o scope do profiler como `Radiance cache (update)` separado do resolve.
- O passe deve rodar depois do G-buffer e antes dos consumidores de iluminação que consultam o cache. Ele consulta sempre `Resolved` do frame anterior; as escritas vão somente para `Accum`, e o resolve continua depois de todos os traces. Não é necessária uma barreira entre update e query sobre `Resolved`, apenas ordenar/visibilizar `Accum` antes do resolve.

### Novos parâmetros mínimos

- `UpdateFraction`, default `0.04`.
- `UpdateMaxBounces`, default `4`.
- `RenderMaxBounces`, inicialmente preserva o comportamento atual do ReSTIR GI.
- `MinCacheableRoughness` ou regra equivalente.
- `UsePreviousCacheAtTerminal`, default ligado.
- `UseDDGIBootstrap`, somente debug, default desligado.

### Arquivos prováveis

- `Engine/Include/Smile/Graphics/RadianceCache.h`
- `Engine/Source/Graphics/RadianceCache.cpp`
- `Engine/Source/Graphics/Renderer.cpp`
- novo `Shaders/GI/RadianceCacheUpdate.cs.hlsl`
- `Shaders/GI/RadianceCache.hlsli`
- `Shaders/GI/PathTracingCommon.hlsli`
- `Shaders/CMakeLists.txt`

### Gate de saída

- Com DDGI desligado e query de render desligada, o cache acumula radiância válida apenas pelo update dedicado.
- Desligar todos os antigos consumidores de update não faz a ocupação parar de crescer.
- A taxa de paths e profundidade média batem com os parâmetros.
- Não há NaN/Inf, overflow do acumulador ou GPU hang em emissivos extremos.
- O custo do update aparece isolado no profiler.

## Fase 4 — Tornar o resolve e a confiança adequados ao produtor dedicado

### Objetivo

Publicar um cache estável, responsivo e mensurável antes de habilitá-lo como sinal principal.

### Trabalho

- Manter o limite de 64 amostras e a evicção após 64 frames como ponto inicial.
- Manter o refresh escalonado por checksum para evitar tempestades periódicas.
- Verificar matematicamente a média após atingir o teto. O peso histórico deve permanecer limitado; não deixar a célula congelar com um `prevSamples` efetivo maior que o armazenado.
- Definir estados explícitos da célula:
  - ausente;
  - presente sem amostra;
  - aquecendo;
  - confiável;
  - precisa refresh;
  - stale/evictada.
- A confiança inicial pode usar somente `sampleCount`, `age` e elegibilidade geométrica. Variância/segundo momento só entra se os testes mostrarem necessidade; não aumentar 16 MiB por buffer sem evidência.
- Criar aquecimento global do cache:
  - `Filling`: update ativo, queries de render ainda não fazem early-out;
  - `Active`: cobertura/convergência mínimas atingidas ou janela de warm-up completada;
  - `Resetting`: mudança de chave, cena ou teleport; queries fechadas até o resolve de reset.
- Preservar o controle manual update/query para A/B, mas o modo de produção deve ser automático.
- Expandir estatísticas:
  - tentativas e falhas de inserção;
  - probes percorridos por busca, média e máximo;
  - misses por chave, zero samples, refresh, segmento curto e cone estreito;
  - updates aceitos/descartados;
  - paths lançados e profundidade média/máxima;
  - terminal por sky, emissivo, cache anterior ou limite de bounce.
- Expandir visualização com `Age`, `Confidence`, `Fallback source` e, se barato, `Path depth`.

### Gate de saída

- Em câmera estática, ocupação fica preferencialmente entre 20% e 70%; acima disso exige ajuste de célula/LOD/capacidade antes de seguir.
- Falha por bucket cheio deve ser residual e explicitamente reportada; alvo inicial menor que 0,1% das inserções, ideal menor que 0,01%.
- A convergência responde a luz acesa/apagada e Time of Day sem congelar por dezenas de segundos.
- Teleport e troca de cena não mostram radiância do local anterior.
- O cache pode aquecer com DDGI completamente desligado.

## Fase 5 — Usar SHaRC/WRC como terminador primário do ReSTIR GI

### Objetivo

Fazer o `FReSTIRGI` produzir seu `Lo` secundário a partir do cache independente e deixar o DDGI somente no ramo de miss.

### Trabalho

- No trace inicial de ReSTIR GI:
  1. traçar da superfície primária para o ponto secundário;
  2. classificar o hit e formar a chave;
  3. consultar o cache antes da avaliação cara de material quando o path for elegível;
  4. em hit confiável, usar a radiância do cache como `Lo` e encerrar;
  5. em miss, avaliar material, direto e emissivo do hit;
  6. somente no terminal do miss consultar o fallback DDGI, se disponível;
  7. sem DDGI, usar ambiente/zero de maneira explícita.
- O render path não escreve em `Accum`.
- O cache não pode retornar radiância direcional para mirror/narrow glossy. Manter o gate por cone e testar o sweep de roughness.
- Preservar a distância do primeiro hit para NRD. Um cache hit muda a fonte da radiância, não a geometria visível que guia o denoiser.
- Limitar a idade do reservoir de ReSTIR GI a oito frames no preset SHaRC, seguindo a solução prática do Cyberpunk. Não tornar isso literal global se um A/B demonstrar regressão; o knob já existe.
- Religar validação periódica somente se o limite de idade + atualização do cache não responderem a luz dinâmica. Evitar pagar dois mecanismos sem necessidade.
- Invalidar ReSTIR GI, NRD/RR e TAA na borda em que o produtor primário muda entre DDGI e SHaRC.
- Manter temporal e espacial do `FReSTIRGI` inalterados no primeiro commit desta fase. Ajustes de bias/resampling vêm em commit separado.

### Gate de saída

- Com DDGI desligado, ReSTIR GI continua iluminando áreas multi-bounce após o warm-up do cache.
- Com fallback DDGI habilitado, sua taxa de uso cai conforme o cache aquece e estabiliza; a telemetria prova isso.
- Com cache confiável, desligar o update do DDGI não altera significativamente regiões já cobertas, exceto consumidores volumétricos.
- Não há leitura de DDGI no cache-update path.
- Não há dupla contagem: cache hit não soma novamente DDGI no mesmo hit.
- ReSTIR spatial/temporal não cria fireflies novos nem ghosting de iluminação acima do baseline.

## Fase 6 — Formalizar DDGI como fallback e manter volumetria

### Objetivo

Transformar a prioridade em configuração explícita do renderer e reduzir o custo do DDGI sem removê-lo.

### Trabalho

- Introduzir modos de alto nível, com nomes equivalentes a:

```cpp
enum class EIndirectPrimary {
    ReSTIR_SHaRC,
    DDGI,
    Off
};

enum class EIndirectFallback {
    DDGI,
    Environment,
    Black
};
```

- `UseGI` não deve continuar significando “DDGI ligado”. Ele significa que o domínio indireto está ativo.
- O deferred usa a saída de ReSTIR GI quando `ReSTIR_SHaRC` está ativo. O atlas DDGI não é somado como outra GI principal.
- Fog/volumetric scattering pode continuar lendo irradiância DDGI. Documentar essa exceção: é um consumidor volumétrico, não o estimador principal de superfície.
- Manter o DDGI aquecido quando ele for o fallback selecionado. Só depois medir redução de orçamento:
  - menos probes atualizadas por frame;
  - menor frequência de relight/update;
  - cascatas distantes/coarser;
  - prioridade para regiões de miss do cache, se houver um mapeamento barato.
- Se o custo do fallback continuar alto, avaliar o desenho do AC Shadows em change set próprio:
  - G-buffer persistente por probe;
  - trace/refresh de visibilidade separado do relight;
  - relight mais frequente que o trace completo;
  - metadado de escala de luminância para Time of Day em probes não selecionadas.
- Não expandir o DDGI da Smile para radiância direcional só para copiar o AC Shadows. Fazer isso apenas se um consumidor real justificar o custo; para fallback difuso/fog, irradiância continua sendo o produto adequado.
- Não introduzir fallback artístico diferente. Intensidade do DDGI fallback deve representar a mesma iluminação, sem multiplicador usado para mascarar diferenças.
- UI deve mostrar claramente:
  - primário;
  - fallback;
  - estado do cache: filling/active/resetting;
  - porcentagem de queries atendidas pelo cache;
  - porcentagem atendida por DDGI/ambiente;
  - custo de update/resolve e VRAM.
- Preservar “DDGI primary” como modo de rollback até o fechamento final.

### Gate de saída

- O modo padrão pode ser `ReSTIR_SHaRC + DDGI fallback` sem ordem implícita em flags desconectadas.
- O modo DDGI primary reproduz o baseline para rollback.
- Desligar o volume DDGI não impede setup, resize, denoise ou execução do ReSTIR GI.
- Fog continua estável em todos os modos; quando DDGI está ausente, usa fallback explícito em vez de descriptor inválido.

## Fase 7 — Qualidade, scheduling e otimizações inspiradas na idTech8/AC Shadows

### Objetivo

Reduzir custo e memória depois que o estimator estiver correto.

### Trabalho

- Substituir gradualmente o full-screen early-out do updater por geração/compactação de work items.
- Medir uma arquitetura de hit packets:
  - trace de visibilidade produz hit compacto, usando 128 bits como referência de orçamento da idTech8, não como requisito cego;
  - hash/deduplicação identifica células ativas;
  - shading acontece uma vez por célula/material quando isso for estatisticamente correto;
  - indirect dispatch por material evita megashader divergente;
  - aproximadamente 20 mil células ativas/frame e 14 MiB são números de referência da idTech8, não metas automáticas para a Smile.
- Essa otimização da idTech8 não deve ser aplicada se quebrar backpropagation ou introduzir bias não medido. Ela é um segundo produtor possível, não requisito da correção SHaRC.
- Medir update em async compute somente após definir dependências de TLAS, G-buffer, ReGIR, cache resolve e consumidores. Não colocar em compute por intuição.
- Considerar time slicing por tiles e prioridade de pixels em disoclusão/miss, inspirado no relight seletivo do AC Shadows.
- Medir uma hierarquia de consulta/trace barata antes do BVH completo:
  - screen-space trace validado para localizar/reaproveitar o hit secundário;
  - SHaRC/WRC para radiância world-space;
  - DDGI somente em miss/cache frio;
  - ambiente/zero quando DDGI não existir.
- O screen-space path é otimização, não fonte de verdade: rejeitar por depth/normal/material/motion e cair para BVH quando houver dúvida. ReSTIR deve continuar recebendo um ponto secundário geometricamente válido.
- Medir presets de capacidade `2^18`, `2^19`, `2^20` e `2^21`. `2^22` só entra com justificativa de cena e VRAM.
- Medir base cell/LOD a partir do estado conhecido `0,50 m / 6 m`; não voltar ao preset que saturou 95,2% da tabela.
- Se a variância for necessária, avaliar buffer auxiliar de luminância/segundo momento ou encoding compacto. A decisão precisa trazer delta de qualidade e MiB.
- Avaliar atualização adaptativa:
  - mais paths em disoclusão, cache miss, alta idade ou luz dinâmica;
  - menos paths em células convergidas;
  - teto global rígido para custo previsível.

### Gate de saída

- Otimização individual tem A/B de custo e qualidade; nenhuma “melhoria” entra apenas por reduzir ms numa câmera.
- Meta inicial de custo incremental para update + resolve: no máximo 1,5 ms em 1440p na GPU-alvo escolhida pelo projeto, ou orçamento explicitamente redefinido com evidência.
- O frame não apresenta picos periódicos de refresh/evicção.
- VRAM padrão do cache permanece próxima dos 36 MiB atuais, salvo justificativa medida.

## Fase 8 — Hardening do ReSTIR GI e denoising

### Objetivo

Adequar o reúso e o denoiser ao novo perfil de ruído sem mascarar erros do cache.

### Trabalho

- Manter direta e indireta em instâncias RELAX separadas. O capítulo do Cyberpunk mostra que os perfis de variância são incompatíveis para tuning conjunto.
- Confirmar que o pack indireto mantém diffuse GI e reflections/specular nos canais corretos, com hit distances coerentes.
- Fazer sweep controlado de:
  - reservoir max age, começando em 8;
  - M cap;
  - boiling filter;
  - temporal bias correction;
  - spatial count/radius;
  - firefly clamp;
  - history length do RELAX.
- Não usar clamp de firefly para esconder energia errada do cache. Primeiro localizar célula, path e fonte de fallback via debug.
- Se futuramente o mesmo reservoir transportar specular indireto, reduzir o raio espacial com roughness e desligar spatial abaixo de roughness `0.1`, como no Cyberpunk.
- Sem adotar ReSTIR PT, medir opcionalmente a unificação do trace inicial difuso/especular no estilo Cyberpunk: escolher um lobo por probabilidade de energia, corrigir o PDF da mistura e alimentar os pipelines/guide buffers correspondentes. Manter atrás de toggle e somente prosseguir se reduzir trace/VRAM sem contaminar o cache não direcional.
- Testar DLSS RR separadamente de NRD e None. Mudança de produtor exige reset dos histories correspondentes.
- Garantir que motion/disocclusion invalidem reservoir sem necessariamente limpar o cache world-space; teleport/troca de cena limpam ambos.

### Gate de saída

- Sequências com Time of Day, emissivo animado e luz móvel não deixam lag visível prolongado.
- Disoclusão não espalha célula brilhante em blob pelo pre-pass do denoiser.
- NRD, RR e None não leem recursos liberados ou históricos de outro modo.
- O modo cru continua disponível para avaliar o estimator antes do denoiser.

## Apêndice opcional — RTXDI 3.0 ReSTIR PT (fora do escopo padrão)

### Objetivo

Substituir ou complementar o ReSTIR GI de um ponto por resampling de paths completos somente se houver autorização explícita, hardware-alvo adequado e ganho que justifique a complexidade. O Opus não deve executar este apêndice como continuação automática das fases 0–9.

### Por que é uma fase separada

O `FReSTIRGI` atual armazena e reconecta um ponto secundário. ReSTIR PT grava informação de paths, faz random replay/hybrid shift e exige integração profunda com o path tracer via `RTXDI_PathTracerContext` e callbacks `RAB_*`. Isso não é um upgrade local do reservoir existente.

### Trabalho

- Integrar primeiro apenas o runtime RTXDI ao CMake, sem alterar a imagem.
- Usar o Full Sample local como referência para `RAB_PathTrace`; não inventar o contrato a partir do header.
- Implementar callbacks usando o mesmo material, luz, emissivo, ambiente, offsets e masks do path updater validado nas fases anteriores.
- Inserir a consulta SHaRC como terminal permitido do `RAB_PathTrace`, preservando o registro correto dos bounces no `RTXDI_PathTracerContext`.
- Fazer a migração em degraus:
  1. initial sampling sem reúso, comparado ao path updater;
  2. temporal replay;
  3. spatial/hybrid shift;
  4. boiling/bias correction;
  5. guide buffers e denoising;
  6. performance e memória.
- Manter `FReSTIRGI` atrás de toggle até o ReSTIR PT superar seus gates.
- Tratar mirror/PSR e Russian roulette conforme a documentação do RTXDI; não terminar estocasticamente bounces de mirror que alimentam guide buffers incompatíveis.
- Não compartilhar layouts de reservoir por conveniência. Criar recursos próprios do ReSTIR PT e contabilizá-los em VRAM.

### Gate de saída

- Sem reuse, o path tracer RTXDI produz referência equivalente ao path tracer comum.
- Temporal e spatial podem ser ligados separadamente e cada um possui captura de regressão.
- O ganho de estabilidade/qualidade é visível no sinal cru, não apenas depois do denoiser.
- Custo e VRAM cabem no preset alvo.
- Só então decidir se `FReSTIRGI` vira fallback legado ou é removido em outro change set.

## Fase 9 — Testes automatizados, documentação e mudança de default

### Testes de CPU

- Criar `SmileRadianceCacheMathTests` sem dependência do runtime D3D12 quando possível.
- Espelhar/testar:
  - cálculo de LOD e cell size;
  - quantização de posição e octante da normal;
  - determinismo do hash/checksum;
  - probing com vazio, colisão e tombstone;
  - média temporal com teto de 64;
  - idade, refresh e evicção;
  - composição de confiança.
- Centralizar constantes compartilhadas ou adicionar assertions para impedir drift silencioso entre C++ e HLSL.

### Testes de GPU/captura

- Câmera estática por pelo menos 128 frames.
- Travelling contínuo para testar LOD e churn.
- Teleport.
- Parede fina.
- Roughness sweep.
- Emissivo liga/desliga.
- Sol muda de direção/intensidade.
- Objeto dinâmico grande atravessa região iluminada.
- DDGI indisponível desde o setup.
- DDGI desligado durante execução.
- Resize, mudança de render scale e hot reload de HLSL.
- NRD, DLSS RR e None.

### Critérios finais

- Zero D3D12 validation errors, device removal, NaN/Inf ou descriptor inválido.
- SHaRC/WRC produz multi-bounce sem DDGI.
- DDGI aparece apenas nos contadores de fallback/volumetria quando o primário é SHaRC.
- Cache hit não paga gather DDGI.
- Cache miss possui fallback determinístico e observável.
- Mudança de cena/teleport não vaza histórico.
- Direta permanece equivalente ao baseline ReSTIR DI.
- Indireta crua melhora ou mantém estabilidade em relação ao baseline dentro do orçamento definido.
- O modo DDGI primary continua funcional por pelo menos um ciclo de estabilização.
- Somente após todos os gates, mudar o default para `ReSTIR_SHaRC + DDGI fallback`.

## Sequência recomendada de commits

1. `gi: add reproducible SHaRC/DDGI A-B modes and counters`
2. `rt: move shared instance bindings out of DDGI ownership`
3. `gi: decouple ReSTIR GI and indirect NRD setup from DDGI readiness`
4. `gi: split hit shading from indirect fallback policy`
5. `gi: add sparse radiance-cache path update pass`
6. `gi: backpropagate multi-bounce radiance into WRC`
7. `gi: add cache confidence, warm-up state and miss telemetry`
8. `gi: make WRC the primary ReSTIR GI terminator`
9. `gi: formalize DDGI as surface fallback and volumetric provider`
10. `gi: tune ReSTIR/RELAX histories for SHaRC signal`
11. `gi: compact and schedule radiance-cache updates`
12. `tests: add radiance-cache math and regression coverage`
13. Apêndice opcional, somente se autorizado: `rtxdi: integrate ReSTIR PT runtime and initial sampling`

O implementador pode dividir mais, mas não deve fundir os commits 2–8 em um único change set.

## Comandos mínimos de validação por fase

```powershell
cmake --build build --config Debug --target Shaders
cmake --build build --config Debug --target SmileEditor
ctest --test-dir build -C Debug --output-on-failure
```

Quando a fase mexer apenas em shader, ainda compilar `SmileEditor` se houver mudança de cbuffer, descriptor table, enum ou settings bridge.

## Pacote que o Opus deve entregar para validação

Ao fechar cada fase, enviar:

- resumo do que mudou e do que deliberadamente não mudou;
- lista de arquivos tocados;
- `git diff --stat` e diff relevante, sem incluir/reverter alterações preexistentes do usuário;
- comandos executados e resultado;
- GPU timings antes/depois;
- VRAM antes/depois;
- valores dos novos contadores;
- capturas cruas e denoisadas nas cenas de regressão;
- limitações conhecidas;
- qual gate da fase ainda não foi provado.

Se um gate falhar, corrigir dentro da mesma fase. Não avançar acumulando dívida estatística ou descriptor/resource lifetime bugs.

## Referências de implementação

- `Fast as Hell — idTech8 Global Illumination`: hash world-space compacto, deduplicação antes do shading, work lists por material e forte controle de orçamento.
- `Raytracing the world of Assassin's Creed Shadows`: atualização temporal seletiva, separação entre dados persistentes e relight, DDGI robusto como cobertura ampla e suas limitações de leaking/distribuição uniforme.
- `The Evolution of the Real-Time Lighting Pipeline in Cyberpunk 2077`: combinação ReSTIR GI + SHaRC, update em aproximadamente 4% dos pixels com até quatro bounces, resolve temporal, terminação por voxel/cone e separação de denoising direto/indireto.
- `D:/Engines/RTXDI-main/Doc/RestirGI.md`
- `D:/Engines/RTXDI-main/Doc/RestirPT.md`
- `D:/Engines/RTXDI-main/Samples/FullSample`: referência obrigatória somente se o apêndice ReSTIR PT for autorizado.

