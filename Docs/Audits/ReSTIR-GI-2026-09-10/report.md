# Auditoria ReSTIR GI — SmileEngine
Data: 10/09/2026 · Código: d079054ee2df61fd43bcccc4161a889039c67763

## Parecer

O núcleo de reservoir está coerente com o ReSTIR GI e com o RTXDI local. A integração ainda não passa como referência de transporte fisicamente correto: há **cinco achados prioritários** no empacotamento de normais, composição, visibilidade e reconstrução temporal. O caminho padrão é deliberadamente enviesado, com radiância de cache, limites de energia e várias heurísticas.

Não alterei shaders nem C++ de produção. Esta entrega contém análise, provas numéricas e evidência de compilação/execução. Não é uma medição de convergência contra path tracing de referência.

**P1:** corrigir antes de validar qualidade/energia. **P2:** corrigir no caminho afetado antes de promovê-lo.

## 1. Achados acionáveis

### F5 · P1 · Encoder octaédrico corrompe normais com componente zero

**Evidência:** [DDGICommon.hlsli:127](D:/Engines/SmileEngine/Shaders/GI/DDGICommon.hlsli:127), [ReSTIRReservoir.hlsli:64](D:/Engines/SmileEngine/Shaders/GI/ReSTIRReservoir.hlsli:64) e [GBuffer.hlsli:15](D:/Engines/SmileEngine/Shaders/GBuffer.hlsli:15).

A dobra do hemisfério inferior usa `sign(n.xy)`. Em HLSL, sign(0)=0; a projeção octaédrica exige escolher +1 ou −1, inclusive para zero. Portanto o round-trip de **(0,0,−1) retorna (0,0,+1)**. Outro caso: normalize(1,0,−1) retorna (1,0,0).

Isso não é apenas erro pequeno de quantização. Uma parede plana alinhada ao eixo pode fornecer −Z exato. O G-buffer usa a mesma expressão, então a normal primária pode indicar o hemisfério oposto; no reservoir, n2 corrompida altera o Jacobiano e pode rejeitar sistematicamente conexões válidas. n1 também passa pelo encoder do reservoir.

**Prova executada:** a [sonda](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/normal-probe.cs.hlsl) inclui os headers reais da engine e chama ResPackNormal/ResUnpackNormal com entradas constantes. O DXC otimizado emitiu stores literais de (0,0,+1) e (+1,0,0), registrados na [assembly](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/normal-probe.asm). É evidência do comportamento compilado, sem execução GPU da sonda.

**Correção proposta:** substituir a escolha de sinal por `v>=0 ? +1 : −1` e compartilhar um encoder correto nos consumidores; tratar normal nula explicitamente. O [encoder C++ da própria engine](D:/Engines/SmileEngine/Engine/Include/Smile/Graphics/RayTracing/RTTriangle.h:78) e o [encoder RTXDI](D:/Engines/RTXDI/Libraries/Rtxdi/Include/Rtxdi/Utils/TypePacking.hlsli:41) já fazem essa escolha corretamente.

**Validação:** round-trip dos seis eixos, componentes zero no hemisfério inferior, vetores próximos às costuras e normais nulas; executar também no formato real RGB10A2 do G-buffer e snorm16 do reservoir. Depois validar uma sala de faces alinhadas com initial-only e reuse. Corrigir antes de interpretar imagens de erro do estimador.

### F1 · P1 · Ambiente difuso pode ser somado ao ReSTIR quando DDGI está indisponível

**Evidência:** [DeferredLighting.ps.hlsl:487](D:/Engines/SmileEngine/Shaders/DeferredLighting.ps.hlsl:487), [linha 510](D:/Engines/SmileEngine/Shaders/DeferredLighting.ps.hlsl:510) e [linha 558](D:/Engines/SmileEngine/Shaders/DeferredLighting.ps.hlsl:558).

O primeiro ramo adiciona ambiente atmosférico quando `UseAtmoAmbient && !UseGI`. O ramo de IBL permite o difuso quando `!(UseGI || UseAtmoAmbient)`. Ambos ignoram `UseReSTIR`, que adiciona o gather mais adiante.

A condição é alcançável: o primário ReSTIR continua disponível sem volume DDGI; nesse caso o CPU zera DDGIGridCount.w, mas não desliga SkyAmbientColor.w. Ver [RendererFrame.cpp:372](D:/Engines/SmileEngine/Engine/Source/Graphics/Renderer/RendererFrame.cpp:372), [linha 410](D:/Engines/SmileEngine/Engine/Source/Graphics/Renderer/RendererFrame.cpp:410) e [Renderer.cpp:621](D:/Engines/SmileEngine/Engine/Source/Graphics/Renderer/Renderer.cpp:621).

**Gatilho:** ReSTIR ativo, DDGI sem volume/desligado e ambiente atmosférico ligado; alternativamente IBL difuso ligado com ambiente atmosférico desligado. O ReSTIR já recebe céu nos misses e radiância secundária nos hits. O ambiente primário adicional altera energia e pode iluminar interiores independentemente da visibilidade de GI.

**Correção proposta:** fazer a exclusão do ambiente difuso primário considerar `UseReSTIR` nos dois ramos. Preservar o IBL especular e os consumidores auxiliares de DDGI.

**Validação:** plano Lambertiano sob céu constante, sem DDGI, com ambiente primário on/off e ReSTIR ligado. O resultado difuso não deve variar pelo termo substituído. Acrescentar interior fechado para revelar o preenchimento indevido. A prova atual é de fluxo de código e tabela lógica; não foi feito A/B isolando esse termo na GPU.

### F2 · P1 · Visibilidade final fica acoplada ao passe espacial

**Evidência:** [ReSTIRGITrace.cs.hlsl:441](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGITrace.cs.hlsl:441), [ReSTIRGISpatial.cs.hlsl:211](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGISpatial.cs.hlsl:211), [ReSTIRGI.cpp:405](D:/Engines/SmileEngine/Engine/Source/Graphics/GI/ReSTIRGI.cpp:405).

O passe A resolve a amostra temporal sem um teste final de oclusão. O único teste final está no passe B, condicionado a Visibility. Desligar Spatial elimina o teste mesmo se Visibility estiver ligado. A revalidação do passe A é periódica e nasce desativada, portanto não substitui esse contrato.

Além disso, [os defaults](D:/Engines/SmileEngine/Engine/Include/Smile/Graphics/GI/ReSTIRGI.h:293) ligam Spatial, mas deixam Visibility=false. A amostra nova teve sua conexão traçada; amostras reconectadas não herdam automaticamente essa visibilidade. Testes de posição/normal e Jacobiano não detectam um oclusor entre receptor e amostra.

O RTXDI mantém [visibilidade no final shading](D:/Engines/RTXDI/Samples/FullSample/Shaders/LightingPasses/GI/FinalShading.hlsl:87) como etapa independente. A correção de visibilidade nos pesos MIS é outra operação, com finalidade diferente.

**Correção proposta:** separar “visibilidade final” de “visibilidade da correção MIS”. Resolver a primeira tanto em temporal-only quanto após spatial, evitando o raio duplicado quando possível. Manter um modo rápido sem visibilidade é escolha legítima, mas deve ser identificado como enviesado.

**Validação:** receptor imóvel, ponto secundário iluminado e oclusor fino móvel. Matriz initial-only / temporal-only / temporal+spatial, visibilidade final on/off. Quando bloqueada, a conexão selecionada precisa contribuir zero. Os 8 frames de idade máxima reduzem persistência; não garantem visibilidade no frame atual.

### F3 · P2 · Histórico full-res do half-res pode ficar sem atualização sob movimento

**Evidência:** [ReSTIRGIUpsample.cs.hlsl:163](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGIUpsample.cs.hlsl:163) e [linha 192](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGIUpsample.cs.hlsl:192).

Só pixels da fase atual recebem GI nova. Os demais, passando nos testes geométricos, copiam GIPrevious e retornam. Esse histórico full-res não tem idade, limite de cópias nem mistura obrigatória com a estimativa atual. O MaxAge do reservoir não controla esse buffer.

**Reprodução analítica:** em uma superfície cujo ponto se desloca +1 pixel em X por frame, tome `x(t)=101+t` e `phase.x=t&1`. As paridades são sempre opostas. O ponto nunca é fresh, independentemente da fase Y. Com reprojeção exata e geometria válida, o código pode copiar a mesma iluminação indefinidamente. A simulação anexada produziu **0 atualizações em 256 frames**. Não depende de erro de motion vector.

**Correção proposta:** idade/confiança transportada com o histórico full-res, com refresh obrigatório; alternativamente usar uma contribuição atual com peso positivo e controle de validade. Um cronograma de quatro fases fixo na tela, sozinho, não limita a idade de uma amostra que se move.

**Validação:** pan calibrado para um pixel/frame, plano sem desoclusão e mudança de iluminação. Medir idade da informação e resposta ao degrau. Half-res está OFF por padrão, então o caminho full-res não sofre esse problema. A captura estática bem-sucedida não exercita esse caso.

### F4 · P1 · Emissão direta de mesh lights pode entrar tanto no DI quanto no GI

**Evidência:** [DILightSampling.hlsli:168](D:/Engines/SmileEngine/Shaders/Lighting/DILightSampling.hlsli:168), [DeferredLighting.ps.hlsl:477](D:/Engines/SmileEngine/Shaders/DeferredLighting.ps.hlsl:477), [HitShading.hlsli:184](D:/Engines/SmileEngine/Shaders/GI/HitShading.hlsli:184) e [ReSTIRGITrace.cs.hlsl:210](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGITrace.cs.hlsl:210).

O DI amostra a radiância dos triângulos emissivos e sua saída é somada no deferred. Quando o raio cosseno do GI atinge esse mesmo emissivo, ShadeSurfaceHitEx também inclui sua emissão em Lo. O resolve de GI contribui para o mesmo receptor, sem exclusão da emissão do primeiro hit nem MIS entre essas duas estimativas. O cbuffer GI não recebe uma política de cobertura das mesh lights pelo DI.

**Gatilho:** ReSTIR DI ativo, mesh lights efetivamente no pool e ReSTIR GI ativo, com emissivos visíveis pelo hemisfério do receptor. A sobreposição é do caminho câmera → receptor → emissivo, mesmo que o nome do buffer diga “indireto”. Não significa duplicação de toda a GI, nem das luzes analíticas. Na captura de smoke, ReSTIR DI estava OFF; ela não reproduz F4.

**Correção proposta:** definir cobertura explícita dos caminhos. Excluir apenas emissão diretamente coberta pelo DI, preservando espalhamento no secundário e emissores fora do pool; ou combinar os estimadores com MIS apropriado. O fast path do cache devolve radiância agregada que também contém emissão, então não basta remover o “+ emissive” somente no fallback. É necessário separar/identificar essa componente ou recuperá-la de forma consistente. Também invalidar histórico quando mudar a cobertura.

**Comparação:** [BrdfRayTracing.hlsl do RTXDI:206](D:/Engines/RTXDI/Samples/FullSample/Shaders/LightingPasses/BrdfRayTracing.hlsl:206) controla includeEmissiveComponent antes de armazenar a radiância inicial do GI. A engine precisa de um contrato equivalente compatível com seu cache.

**Validação:** receptor Lambertiano e emissivo difuso grande, sem outras luzes, cache inicialmente desligado/controlado. Medir separadamente DI, BRDF/gather e soma, com energia linear e clamps OFF; depois repetir com cache. Cada técnica pode estimar o mesmo termo isoladamente, mas a combinação não deve somá-lo duas vezes. Achado confirmado por caminhos de código; magnitude visual não medida.

## 2. Contrato matemático

O sinal produzido é difuso desmodulado: **E/π**, multiplicado pelo DiffuseColor do receptor no deferred. A radiância secundária Lo já contém a resposta do material atingido. Não encontrei uma multiplicação duplicada do albedo secundário nem um π extra no núcleo do gather.

Para candidato inicial cosseno-hemisférico, a densidade usada é `pω=cosθ/π`; o alvo é `pHat=luminância(Lo)·cosθ`. Com uma candidata, sem clamps e sem deslocamento numérico, `W=1/pω` e o resolve retorna Lo. Sob céu constante isso é exatamente E/π. O piso da PDF e as origens deslocadas introduzem pequenas diferenças no caso real.

O merge usa `w=pHat_dest·W_origem·M_origem·J`. O Jacobiano é `J=(cos_dest/cos_origem)·(dist_origem²/dist_dest²)`, coerente com a transformação de ângulo sólido por uma mesma área. No exemplo de uma pequena área frontal, distâncias 2 e 4 dão J=0,25. Multiplicar por J está correto nessa convenção; não inverter a razão por uma leitura isolada do pseudocódigo do paper.

A finalização `W=wSum·pi/(piSum·pHat_selecionado)` reproduz a estrutura MIS-like dos headers GI do RTXDI. A segunda passagem espacial avalia a amostra vencedora nos domínios participantes e usa os mesmos M do merge. Em domínios idênticos, a equação reduz a `wSum/(M·pHat)`. O temporal possui essa variante, mas o default usa 1/M.

Referências: [paper original, equações e discussão de viés](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing), [documentação de integração GI](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md), [API de normalização](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/ShaderAPI-RestirGI.md). O PDF original foi baixado integralmente e lido localmente, incluindo seções 4.1–5; o leitor web recusou seu tamanho.

**O que não é bug:** omitir albedo do alvo pode ser uma escolha de importance sampling para o sinal desmodulado; pHat não precisa ser uma PDF normalizada. Limitar M preservando W normalizado é a convenção usada no RTXDI. Não realimentar o temporal com o spatial é uma escolha válida para conter correlação. Não é necessário implementar ReSTIR PT para que esse GI difuso seja útil.

## 3. Cobertura do pipeline

- **Geração:** uma direção cosseno por pixel interno/frame; streams distintos para direção, WRS temporal e taps/WRS espacial. Lo vem de ShadeSurfaceHitEx; céu vem da LUT atmosférica.
- **Transporte secundário:** consulta radiance cache antes de material/shading caro. Em miss, direta solar/local, fallback indireto e emissivo. Isso caracteriza um final gather híbrido ReSTIR+cache, não um path tracer independente usado como ground truth. [HitShading.hlsli:110](D:/Engines/SmileEngine/Shaders/GI/HitShading.hlsli:110).
- **Geometria:** x2 é o hit real; normal secundária é de face, transformada para mundo e orientada contra o raio. Alpha-test e OMM participam do traversal. Máscara gather exclui certos transportes, como transmissão volumétrica pelo vidro; não há equivalência a path tracing completo.
- **Temporal:** motion confiável, superfície anterior full-res, normal compactada, teste radial e de plano; MCap=20; idade máxima base=8 com escalonamento entre 0,75 e 1,25. A permutação é bijetiva no domínio inteiro e rejeita saídas de tela. Não há busca adicional como os cinco candidatos do RTXDI.
- **Espacial:** quatro taps por padrão em raio de 16 pixels full-res; rejeição de normal 0,9 e distância proporcional à câmera; no máximo oito taps. A normalização trabalha sobre os candidatos aceitos. A visibilidade MIS requer até K raios adicionais; final visibility acrescenta um.
- **Armazenamento:** RGBA32F + RGBA32_UINT = 32 bytes/pixel por par de texturas; dois pares para ping-pong, portanto 64 bytes/pixel de histórico. Em 1920×1080 são 126,56 MiB só de reservoirs; em 3840×2160, 506,25 MiB. Half-res reduz aproximadamente a um quarto, com buffers adicionais de reconstrução.
- **CPU/GPU:** offsets relevantes do CB têm static_assert; prefixos dos quatro shaders coincidem. Descriptors temporal/spatial seguem a paridade; t13 é atualizado por frame. Há transições UAV→SRV dos reservoirs, UAV barrier do GI entre A/B e alvo separado no upsample antes da cópia. Não encontrei uma corrida de leitura/escrita nesses trechos. [ReSTIRGI.cpp:354](D:/Engines/SmileEngine/Engine/Source/Graphics/GI/ReSTIRGI.cpp:354).
- **Histórico:** resize invalida a reconstrução; clear do reservoir anterior zera M; todos os pixels válidos do passe A sobrescrevem o destino. O previous surface é fornecido somente com histórico de motion válido.
- **NRD:** pack usa RELAX, radiância linear, hit distance em unidades de mundo e normal/roughness pelo header NRD. MV curUV−prevUV recebe escala negativa no CPU. O resolve atualiza a distância da amostra vencedora após spatial. São contratos coerentes na inspeção; não demonstram ausência de ghosting.
- **Composição:** consumo full-res por Load; difuso recebe albedo, intensidade e GTAO. Folhagem/subsurface têm ramos auxiliares DDGI. Comparar imagem final sem isolar esses termos não mede apenas ReSTIR.

## 4. Aproximações e riscos que não devem virar falsos “passes”

### Limites de energia mudam o estimador e o A/B de denoiser

FireflyMaxRaw=4 e FireflyMax=8 são aplicados tanto a Lo antes de WRS quanto ao resolve. Alternar NRD também altera o limite do produtor, via [ReSTIRGI.cpp:284](D:/Engines/SmileEngine/Engine/Source/Graphics/GI/ReSTIRGI.cpp:284).

Em uma cena analítica de radiância uniforme 16, initial-only, o limite de entrada por si só produz no máximo 4 no raw ou 8 no NRD: perdas de 75% e 50%. São números do exemplo, **não perdas medidas no Bistro**. É impossível exigir convergência ao transporte sem clamp nesse regime. Para avaliar denoising, usar o mesmo limite de entrada nos dois lados; para referência, permitir zero/desligado. Os campos não têm setters públicos correspondentes.

### Correção MIS e revalidação presentes não significam habilitadas

TemporalBiasCorr=false e ValidateInterval=0. A revalidação existente reencontra geometria com tolerância max(2%·distância, 5 cm), pode substituir x2/n2 e atualiza Lo. Ela não armazena identidade completa nem os random numbers do caminho. Reativá-la exige testes próprios; não tratá-la como replay fiel do paper. Guardar W ao atualizar só a radiância não é, por si só, prova de erro: W é um peso de contribuição herdado, não simplesmente o inverso do alvo corrente.

A idade máxima reduz lag do reservoir mas não corrige imediatamente luz/oclusor dinâmico. O histórico do NRD e o da reconstrução half-res são camadas adicionais.

### Jacobiano e suporte

O código rejeita J fora de [0,1;10], usa saturate para impedir reconexão pelo verso e rejeita cos_origem<1e−4. O RTXDI local também rejeita os extremos, mas seu sample ainda limita o valor aceito a [1/3;3]. **Os algoritmos não são numericamente idênticos.** A rejeição dependente da amostra é uma heurística; não prova ausência de viés só por evitar clamp.

### Céu representado como ponto finito

Misses armazenam x2=origem+direção·MaxRayDist e n2=−direção. Ao reutilizar entre receptores, a direção muda, mas Lo do céu permanece a antiga. Isso aproxima um ambiente distante por geometria finita. Registrar explicitamente amostras de ambiente e tratar seu domínio direcional evita falsa paralaxe. O impacto depende de deslocamento/MaxRayDist e do contraste angular da LUT; não foi quantificado aqui.

### Radiância direcional e cache

O secundário usa GGX e fallback aproximado, mas o reservoir guarda apenas RGB. A roughness mínima=0,5 reduz problemas de lóbulos estreitos; não torna o espalhamento Lambertiano. Cache atualizado ou fallback DDGI pode mudar enquanto a amostra permanece no reservoir. O método não deve ser usado como prova de correção de caustics/especular de baixa roughness.

### Compressão, normal nula e extremos numéricos

RGB9E5 foi consistente nos valores representáveis testados, mas um expoente compartilhado perde canais fracos: (8;0,001;0,001) vira (8;0;0). A alegação de ausência de desvio de matiz no comentário é forte demais.

Reservoir sem contribuição pode chegar ao pack com n2=0. DDGI_OctEncode divide pela norma L1 sem tratar zero; esse caminho merece sanitização explícita e teste HLSL/GPU. W=0 normalmente impede contribuição e **não há evidência aqui de NaN chegando à imagem**. Ao contrário do helper RTXDI, o Jacobiano da Smile não possui sanitização explícita de NaN/Inf. Não classifiquei essas observações como artefatos visuais comprovados.

### Fallback half-res e testes existentes

O fallback bilateral retorna o vizinho “menos ruim” mesmo quando todos têm peso desprezível, sem um limiar de compatibilidade para esse último retorno. Isso pode espalhar luz em descontinuidades; precisa de cena de teste.

Os scripts existentes de half-res comparam PNGs finais e repetibilidade. São úteis para regressão visual, mas não substituem comparação linear HDR de GI isolada. A bridge GI não expõe toda a matriz de temporal/spatial/clamp/revalidação necessária para um ensaio automatizado do estimador.

## 5. Referências locais e diferenças reais do RTXDI

Referência principal local: **D:/Engines/RTXDI**, commit **1b55517b74b7c9c54eda44be138f75a97f1fee60**. Também conferi D:/Engines/RTXDI-main: não é um checkout Git, e os arquivos da pasta Include/Rtxdi/GI comparados não apresentaram diferenças de conteúdo pelo git diff --no-index.

Arquivos usados: GI/Reservoir.hlsli, TemporalResampling.hlsli, SpatialResampling.hlsli; Utils/Math.hlsli; sample RAB_SpatialHelpers.hlsli, RAB_LightSampling.hlsli e GI/FinalShading.hlsl.

A Smile acompanha a estrutura de WRS, merge com M/W/J e normalização MIS-like. As diferenças relevantes são alvo apenas difuso desmodulado, pesquisa temporal menor, heurísticas geométricas próprias, RGB9E5 em vez de LogLuv, M/idade em 16 bits, saída espacial não persistida, visibilidade final acoplada ao spatial e ausência de mistura final com candidato BSDF para glossy. Essa última é compatível com a separação de reflexões da Smile.

O RTXDI é referência de engenharia, não oráculo livre de aproximações. Seu próprio bridge faz clamp do Jacobiano e o sample oferece visibilidade opcional. Copiar seus defaults não substitui medir a política escolhida.

## 6. Papers posteriores: o que vale para esta engine

**Prioridade 1 — seleção espacial por compatibilidade.** [Compatibility-Guided Neighbor Selection for ReSTIR, 2026](https://research.nvidia.com/labs/rtr/publication/junkins2026compatibility/) é o candidato mais direto: pré-seleciona vizinhos compatíveis antes do reuse caro. Casa com a Smile, que hoje usa quatro taps uniformes e rejeição binária. O trabalho relata redução simultânea de erro e correlação; os ganhos publicados não são previsão para o Bistro. Avaliar depois de corrigir os achados de energia/visibilidade.

**Prioridade 2 — melhor reutilização em desoclusões.** [Stochastic Pairwise MIS, 2026](https://research.nvidia.com/labs/rtr/publication/hedstrom2026stochastic/) permite considerar muitos vizinhos com distribuição focada nos úteis. Pode ajudar quando poucos reservoirs novos contêm contribuição. Exige implementar os pesos da seleção; aumentar K ou escolher o vizinho mais brilhante não é equivalente.

**Prioridade 3 — reduzir falhas da busca temporal.** [Reservoir Splatting, 2025](https://research.nvidia.com/labs/rtr/publication/liu2025splatting/) projeta amostras anteriores para seus pixels atuais e combina estratégias por MIS. É relevante para movimento/desoclusão, mas requer outra organização de buffers. [Area ReSTIR, 2024](https://research.nvidia.com/publication/2024-07_area-restir-resampling-real-time-defocus-and-antialiasing) integra posição subpixel e lente; é uma evolução para aliasing/DOF, não correção direta do cronograma half-res atual.

**Correlação persistente.** [Decorrelating ReSTIR Samplers via MCMC Mutations, 2024](https://research.nvidia.com/labs/rtr/publication/sawhney2024decorrelating/) intercala mutações com resampling, reduzindo duplicação/correlação. A Smile guarda pouca informação de caminho; não basta trocar seu RNG. [Conditional Resampled Importance Sampling and ReSTIR, 2023](https://research.nvidia.com/labs/rtr/publication/kettunen2023conditional/kettunen2023conditional.pdf) fundamenta reúso de subcaminhos condicionado e final gather com menor correlação. O gather atual consultar SHaRC/DDGI não o torna uma implementação desse método.

**Fundação e evolução para transporte especular.** [GRIS, 2022](https://research.nvidia.com/publication/2022-07_generalized-resampled-importance-sampling-foundations-restir) fornece a teoria para pesos de contribuição, domínios distintos e amostras correlacionadas; é a base para revisar novos shifts. [ReSTIR PT Enhanced, 2026](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/) aborda seleção recíproca, footprints e mapas de duplicação, além de integração DI/GI. Os ganhos relatados de 2–3× são de ReSTIR PT no trabalho. Migrar exige representação/replay de paths, critérios de reconexão e bridge; não é uma troca local de Reservoir.

**World-space reuse:** [WorldSpace ReSTIR GI, 2023](https://wangningbei.github.io/2023/ReSTIR_files/paper_ReSTIRGI.pdf) e [World-Space Spatiotemporal Reservoir Reuse, 2021](https://gpuopen.com/download/SA2021_WorldSpace_ReSTIR.pdf) são leituras para reduzir dependência da tela. Nesta auditoria foram localizados e triados, sem leitura integral desses dois PDFs. A Smile já usa cache em mundo, mas cache de radiância e reservoir de caminhos são estruturas diferentes; não há evidência para recomendar substituir SHaRC por eles agora.

## 7. O que foi executado

1. Compilação do target Shaders, Release.
2. Recompilação forçada dos quatro shaders GI com DXC 1.9.2602.24, usando os perfis 6.9/6.6 do projeto e saídas separadas em tmp/restir-gi-audit: **4/4 sem erro**.
3. Build SmileEditor Release: **concluído**. Warnings C4324 de padding/alinhamento, sem falha de build.
4. CTest: RadianceCacheMath, TemporalTransformWarp, DescriptorLifetime, SlotKeyedHistory, TemporalIdentityTable e TemporalResolveWarp: **6/6 passaram**. Os testes WARP exercitam componentes de histórico/identidade; não os ray queries ReSTIR GI.
5. [Provas numéricas em CPU](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/numerical-audit.mjs): **15 verificações**. Incluem identidade inicial, conversão geométrica independente, redução MIS, expectativa RIS discreta com candidatos pretos, contraexemplo half-res e compressão. [Resultados](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/numerical-results.json). Não executam o HLSL de produção; as provas de limites/branches não são testes de regressão GPU.
6. Sonda HLSL adicional: confirmou dois round-trips incorretos de normal no código compilado (F5), sem despachar a sonda na GPU.
7. Sessão de integração Bistro com o build **d079054-dirty**: ReSTIR+SHaRC efetivo, fallback DDGI, NRD. Capturas científicas com 32 frames de warmup; full-res 1298×775 e half-res 649×388 com saída 1298×775. [Metadados completos](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/runtime.json).
8. Inspeção visual: ambas renderizaram a cena e mantiveram a composição geral. Log da sessão p19824 sem ocorrências dos padrões de erro GPU/API pesquisados. Isso é smoke de funcionamento, não execução com GPU-based validation nem medida de correção física. A sessão criada para a auditoria foi encerrada; half-res foi restaurado para OFF.

A primeira execução usou um binário identificado como f4f84c5-dirty e foi descartada como evidência da revisão atual. O sufixo dirty também aparece no build final; havia dois arquivos não rastreados preexistentes na raiz. Nenhum diff rastreado de produção foi introduzido.

As capturas estão em [full-res](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/bistro-full.png) e [half-res](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/bistro-half.png). O snapshot de GPU salvo não é benchmark: faltam amostragem repetida, regime comum e intervalos de confiança. Não atribuí os artefatos F1–F5 a essas imagens estáticas.

## 8. Ordem sugerida para fechar a validação de qualidade

1. Corrigir o encoder de normais F5, a sobreposição de emissivos F4, o ambiente F1 e separar visibilidade final da correção MIS; proteger temporal-only.
2. Acrescentar modo de referência: initial-only, clamps OFF, sem NRD/TAA/DLSS/GTAO, produtor secundário controlado e captura linear HDR. Expor os mesmos knobs pela bridge com readback.
3. Validar céu constante, área emissiva, duas superfícies Lambertianas, descontinuidade de normal, oclusor fino e materiais pretos. Comparar médias de ensembles e intervalos de confiança com path tracing de mesmo transporte, não apenas frames isolados.
4. Medir degraus de iluminação, oclusor móvel, câmera em movimento e perda de histórico. Incluir o contraexemplo F3 e limites de idade em cada camada.
5. Isolar NRD com produtor idêntico; medir GI crua e denoised. O A/B atual raw/NRD muda também o clamp.
6. Só então experimentar neighbor compatibility/pairwise MIS e medir erro, correlação temporal e custo GPU. Preservar baseline e configurações explícitas.

**Limite desta entrega:** a revisão estática, a comparação com fontes e os checks de build/smoke estão concluídos. A aprovação quantitativa de convergência, vieses e qualidade em movimento permanece pendente dos ensaios acima; o pipeline atual não oferece um modo de referência automatizado suficiente para certificá-la sem trabalho adicional.
