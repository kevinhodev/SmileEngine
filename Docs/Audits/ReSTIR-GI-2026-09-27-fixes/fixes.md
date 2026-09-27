# Correções da auditoria ReSTIR GI

27/09/2026. Base: `44bb75810f43b198b9104cbb2b9575c3c0808f1e`.
Build executado antes do commit: `44bb758-dirty`, contendo estas alterações.

Os cinco achados da [auditoria original](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-10/report.md) receberam correções de produção.

## Alterações

- **F5 — normais:** encoder octaédrico compartilhado usa sinal não nulo na dobra e define +Z para entrada nula. G-buffer e DDGI/reservoir usam o mesmo helper. Corrige −Z exato e as costuras do hemisfério inferior. [Octahedral.hlsli](D:/Engines/SmileEngine/Shaders/Octahedral.hlsli).
- **F1 — composição:** ambiente difuso atmosférico e IBL difuso deixam de ser somados quando ReSTIR GI está ativo, inclusive sem DDGI. O IBL especular e os ramos auxiliares de folhagem/subsurface foram preservados. [DeferredLighting.ps.hlsl](D:/Engines/SmileEngine/Shaders/DeferredLighting.ps.hlsl).
- **F2 — visibilidade:** visibilidade final ligada por padrão e separada da visibilidade opcional no denominador MIS. O passe temporal verifica a conexão quando escolhe histórico e não haverá passe espacial; com spatial, o teste final ocorre apenas no spatial. O teste não grava peso zero no histórico temporal. Ambos compartilham os offsets, alpha-test, OMM e máscara de ray query. O toggle do editor agora se chama “Visibilidade final”; desligá-lo continua permitindo vazamentos. [ReSTIRGIVisibility.hlsli](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGIVisibility.hlsli), [trace](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGITrace.cs.hlsl), [spatial](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGISpatial.cs.hlsl).
- **F3 — half-res:** reprojeção geometricamente válida mistura 25% da reconstrução bilateral atual com 75% do histórico. Pixels da fase atual continuam recebendo a amostra nova inteira. Mesmo quando um ponto móvel perde todas as fases, o peso da iluminação antiga cai para 1,0023% após 16 atualizações e aproximadamente 10⁻⁸ após 64. Isso limita a persistência; não introduz uma textura de idade. [ReSTIRGIReconstruction.hlsli](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGIReconstruction.hlsli), [upsample](D:/Engines/SmileEngine/Shaders/GI/ReSTIRGIUpsample.cs.hlsl).
- **F4 — DI/GI:** o gather recebe a cobertura efetiva do DI: DI ativo, pool publicado, mesh lights habilitadas e candidatas maiores que zero. Uma busca pelo instance ID identifica o triângulo no domínio original, preservando a identidade após compactação. Fluxo inválido/nulo, radiância compactada nula, geometria degenerada e hits fora do pool não são excluídos. Hits cobertos omitem somente a emissão direta na resposta do gather; a consulta ao cache agregado é contornada nesses hits, e o espalhamento é calculado pelo shading/fallback existente. A atualização do cache conserva radiância completa, inclusive emissão, para seus demais consumidores e rebotes. Mudanças de cobertura invalidam reservoirs, reconstrução e NRD indireto, usando o domínio de histórico existente da engine. [MeshLightCoverage.hlsli](D:/Engines/SmileEngine/Shaders/Lighting/MeshLightCoverage.hlsli), [HitShading.hlsli](D:/Engines/SmileEngine/Shaders/GI/HitShading.hlsli), [integração](D:/Engines/SmileEngine/Engine/Source/Graphics/Renderer/RendererPasses.cpp).

## Verificação executada

Build Release de `SmileEditor`, incluindo shaders normais e instrumentados, concluído. `git diff --check` sem erro. O SDK do Windows precisou de execução fora do sandbox de arquivos; a primeira tentativa restrita falhou por acesso negado ao SDK.

**7/7 testes CTest passaram:** RadianceCacheMath, TemporalTransformWarp, DescriptorLifetime, SlotKeyedHistory, TemporalIdentityTable, TemporalResolveWarp e o novo ReSTIRGIRegressionWarp.

O [teste novo](D:/Engines/SmileEngine/Tests/ReSTIRGIRegressionWarpTests.cpp) executa os helpers HLSL de produção em WARP, com entradas em buffers GPU e leitura de resultados: 42 casos de normais (14 entradas × reservoir/G-buffer/DDGI), 10 respostas ao degrau e 13 casos de cobertura de mesh light. Total: **65 casos**. O teste do G-buffer aplica quantização UNORM10 no shader; não é um teste de escrita/leitura do render target RGB10A2 pelo rasterizador. A cobertura usa buffers sintéticos com os layouts C++ de produção. O teste de resposta usa o helper real, sem executar a reprojeção geométrica inteira.

[Log dos testes](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-27-fixes/tests.log).

**7 capturas no Bistro**, com aquecimento de 32 frames e build corrigido:

- Full-res 1298×775, half-res interno 649×388 e full-res sem DDGI: ReSTIR DI/GI ativos, cache ativo, 20.703 triângulos no pool compactado e 8 candidatas emissivas. [Metadados](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-27-fixes/runtime.json).
- Com NRD: pool desligado; pool ligado com zero candidatas; pool não compactado de 26.498 triângulos; pool compactado de 20.703 triângulos. As transições e capturas concluíram. [Metadados](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-27-fixes/transitions.json).

Imagens full/half/sem-DDGI e NRD compactado inspecionadas visualmente. As capturas sem denoiser apresentam o ruído esperado do sinal cru. Não houve ERROR, DEVICE_REMOVED ou erro D3D12 nos logs das duas sessões. Isso não equivale a uma execução com GPU-based validation habilitada. Houve avisos de custo de bake do OMM. Ambas as instâncias de teste foram encerradas e o término confirmado pelo PID.

[Captura com NRD](D:/Engines/SmileEngine/Docs/Audits/ReSTIR-GI-2026-09-27-fixes/bistro-nrd.png).

## Limites e custos

A visibilidade final acrescenta um raio de conexão por pixel com contribuição, quando necessário; a visibilidade MIS permanece opcional e desligada por padrão. Não foi medido um delta confiável de desempenho antes/depois.

A mistura half-res troca parte da nitidez temporal pela resposta obrigatória à iluminação atual. Nas bordas ela herda as aproximações do fallback bilateral existente; não elimina todo ghosting.

O DI existente representa emissão texturizada pela média do triângulo e RGB9E5. A partição elimina a soma das duas estimativas do mesmo caminho, mas não torna essa representação equivalente à emissão pontual de um path tracer. Nos emissores cobertos, evitar o cache agregado muda também a qualidade/custo da estimativa de espalhamento: ela passa pelo shading e fallback existentes, em vez do terminal do cache. A radiância completa continua no cache para os outros caminhos.

Não foi medida convergência contra path tracing nem executado o A/B analítico de energia com clamps desligados. Também não foi executada uma cena controlada de oclusor móvel com temporal-only; esse ramo foi inspecionado e compilado, enquanto o smoke de GPU usou o spatial padrão. Os testes e capturas acima sustentam a correção e integração descritas, sem constituir certificação de transporte sem viés.
