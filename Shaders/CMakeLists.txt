# Compilacao dos shaders HLSL via DXC.
# Cria o target "Shaders" como dependencia do Editor para garantir build na ordem certa.

set(SHADER_OUTPUTS "")

# Instrumentacao de timer nos passes de RT (ver Debug/ShaderTimer.hlsli): as permutacoes "*Timed"
# precisam dos headers HLSL da NVAPI no -I. Sem o SDK, elas simplesmente nao sao compiladas e o
# FShaderTimer nunca fica disponivel — o resto dos shaders nao muda.
set(SMILE_NVAPI_ROOT "D:/Engines/nvapi" CACHE PATH "Raiz do NVAPI SDK (github.com/NVIDIA/nvapi)")
set(SMILE_SHADER_TIMER_AVAILABLE OFF)
if(EXISTS "${SMILE_NVAPI_ROOT}/nvHLSLExtns.h")
    set(SMILE_SHADER_TIMER_AVAILABLE ON)
    set(SMILE_TIMER_FLAGS -D SMILE_SHADER_TIMER=1 -I "${SMILE_NVAPI_ROOT}")
endif()

# Os *NrdPack incluem NRD.hlsli (encode que casa com o decode interno do RELAX). Mesma cache var
# do Engine/CMakeLists.txt — set(... CACHE) nao reescreve entrada existente, entao declarar aqui
# so torna este arquivo independente da ordem dos add_subdirectory. Era um literal
# "D:/Engines/NRD/Shaders" repetido 3x: quem sobrescrevesse SMILE_NRD_ROOT linkava contra um SDK
# e compilava os shaders contra outro.
set(SMILE_NRD_ROOT "D:/Engines/NRD" CACHE PATH "Raiz do NRD SDK")
set(SMILE_NRD_SHADER_INCLUDE -I "${SMILE_NRD_ROOT}/Shaders")

smile_compile_shader(Triangle.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
# Permutacao BINDLESS do TRANSFORM (Fase 4). O mesmo corpo lendo as duas matrizes da tabela
# canonica pelo slot da instancia, em vez do CBV de 256 B amarrado por draw. vs_6_6 porque
# `ResourceDescriptorHeap` e do Shader Model 6.6 — e agora e o VERTEX shader que o usa, ao
# contrario da Fase 3, onde so o pixel shader precisava.
set(SMILE_INSTANCE_BINDLESS_FLAGS -D SMILE_INSTANCE_BINDLESS=1)
smile_compile_shader(Triangle.vs.hlsl vs_6_6 main SHADER_OUTPUTS
    OUTPUT_NAME TriangleBindless ${SMILE_INSTANCE_BINDLESS_FLAGS})
# Sonda de PARIDADE: compoe as quatro matrizes a partir das colunas canonicas, pelo MESMO include
# que o VS usa, e devolve o resultado para o teste comparar com a composicao do C++. A ordem do
# produto e o erro classico desta migracao, e ela compila e desenha dos dois jeitos.
smile_compile_shader(Scene/InstanceTransformProbe.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
# Sonda do COMANDO INDIRETO (Fase 5): o compute monta o argumento pela struct espelhada, e o vertex
# shader relata o que o command processor entregou. Sao duas porque releitura do buffer provaria so
# o produtor -- o layout e ditado pela ordem das entradas da command signature, e nada do lado do
# shader valida isso.
smile_compile_shader(Scene/IndirectArgsProbe.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Scene/IndirectArgsProbe.vs.hlsl vs_6_0 main SHADER_OUTPUTS)

# Deferred shading — geometry pass (preenche o G-buffer MRT) + lighting fullscreen + debug
smile_compile_shader(GBuffer.ps.hlsl          ps_6_0 main SHADER_OUTPUTS)

# Permutacoes BINDLESS do material (Fase 3 do GPU-DRIVEN-PLAN). Mesmo corpo de shader lendo a
# linha canonica pelo heap diretamente indexado em vez do CBV + tabela amarrados por draw; o A/B
# entre as duas vive no MESMO executavel, que e o que a regra 2 do plano exige enquanto o
# fallback existir.
#
# ps_6_6 e nao 6_0: `ResourceDescriptorHeap` e do Shader Model 6.6. O vertex shader nao le
# material e continua em 6_0 — estagios de uma mesma PSO nao precisam do mesmo modelo.
set(SMILE_MATERIAL_BINDLESS_FLAGS -D SMILE_MATERIAL_BINDLESS=1)
smile_compile_shader(GBuffer.ps.hlsl ps_6_6 main SHADER_OUTPUTS
    OUTPUT_NAME GBufferBindless ${SMILE_MATERIAL_BINDLESS_FLAGS})
# Visualizador generico de render targets (absorve o GBufferDebug via DECODE_GBUFFER_FIELD)
smile_compile_shader(DebugView.ps.hlsl        ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(DeferredLighting.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
# Passe forward de translucidos (materiais Blend: vidro etc.), por cima do HDR iluminado
smile_compile_shader(ForwardBlend.ps.hlsl     ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Skybox.vs.hlsl   vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Skybox.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)

# Preview do Editor de Materiais (offscreen 512x512): forward PBR isolado + fundo do HDRI
smile_compile_shader(Preview/MaterialPreview.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Preview/MaterialPreview.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Preview/PreviewSky.ps.hlsl      ps_6_0 main SHADER_OUTPUTS)

# Depth pre-pass com saida de normal geometrica (G-buffer leve p/ o GTAO)
smile_compile_shader(DepthNormal.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
# Variantes masked/two-sided do prepass (alpha-clip, estilo UE masked prepass)
smile_compile_shader(DepthNormalMasked.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(DepthMasked.ps.hlsl       ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(DepthNormalMasked.ps.hlsl ps_6_6 main SHADER_OUTPUTS
    OUTPUT_NAME DepthNormalMaskedBindless ${SMILE_MATERIAL_BINDLESS_FLAGS})
smile_compile_shader(DepthMasked.ps.hlsl       ps_6_6 main SHADER_OUTPUTS
    OUTPUT_NAME DepthMaskedBindless ${SMILE_MATERIAL_BINDLESS_FLAGS})

# Cascaded Shadow Maps (sol direcional) — depth pass das cascatas
smile_compile_shader(Shadow/ShadowDepth.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
# Permutacao de transform da Fase 4. O caster le UMA coluna: sem motion vector e com a
# LightViewProj vindo por cascata, nem PrevTransform nem o CBV de view entram aqui.
smile_compile_shader(Shadow/ShadowDepth.vs.hlsl vs_6_6 main SHADER_OUTPUTS
    OUTPUT_NAME ShadowDepthVsBindless ${SMILE_INSTANCE_BINDLESS_FLAGS})
smile_compile_shader(Shadow/ShadowDepth.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Shadow/ShadowDepth.ps.hlsl ps_6_6 main SHADER_OUTPUTS
    OUTPUT_NAME ShadowDepthBindless ${SMILE_MATERIAL_BINDLESS_FLAGS})

# GI (DDGI) — trace via DXR 1.2 inline ray tracing (RayQuery + OMM, SM 6.9)
# Debug da BVH: raio primario por pixel na TLAS (GPU Zen 3, 7.3.3). cs_6_6 pelo RayQuery + heap
# diretamente indexado (VB/IB bindless do alpha-test e da normal do hit).
smile_compile_shader(Debug/BvhDebug.cs.hlsl    cs_6_9 main SHADER_OUTPUTS)

smile_compile_shader(GI/DDGITrace.cs.hlsl      cs_6_9 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIUpdate.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIUpdateDist.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIRelocate.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGICompactBuild.cs.hlsl    cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGICompactFinalize.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
# ReSTIR GI — final-gather difuso por pixel (reservoir espaciotemporal) sobre o DDGI (cache)
smile_compile_shader(GI/ReSTIRGITrace.cs.hlsl   cs_6_9 main SHADER_OUTPUTS)
smile_compile_shader(GI/ReSTIRGISpatial.cs.hlsl cs_6_9 main SHADER_OUTPUTS)
smile_compile_shader(GI/ReSTIRGIUpsample.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
if(SMILE_SHADER_TIMER_AVAILABLE)
    smile_compile_shader(GI/ReSTIRGITrace.cs.hlsl cs_6_9 main SHADER_OUTPUTS
        OUTPUT_NAME ReSTIRGITraceTimed ${SMILE_TIMER_FLAGS})
endif()
# World radiance cache — hash espacial de radiancia de saida, terminador dos raios secundarios.
# A query mora no ShadeSurfaceHit (RadianceCache.hlsli, sem .cso proprio); o resolve e o passe
# dedicado de update (Fase 3) tem os seus.
# cs_6_6 e nao 6_0: o resolve so usa u0/u1/u2 explicitos, mas inclui o RadianceCache.hlsli, e as
# funcoes de query/update de la acessam ResourceDescriptorHeap (bindless, SM 6.6). Os cinco
# traces que consomem o cache ja sao 6_6.
smile_compile_shader(GI/RadianceCacheResolve.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(GI/RadianceCacheDebug.cs.hlsl   cs_6_6 main SHADER_OUTPUTS)
# Path tracer esparso que ALIMENTA o cache: ~4% dos pixels por frame, sem ler DDGI.
# As duas permutacoes ficam no MESMO binario para o A/B de scheduling da Fase 7 nao comparar
# builds diferentes. Compact e o default; Legacy preserva o full-screen + early-out como controle.
smile_compile_shader(GI/RadianceCacheUpdate.cs.hlsl cs_6_9 main SHADER_OUTPUTS
    -D RCU_COMPACT_DISPATCH=1)
smile_compile_shader(GI/RadianceCacheUpdate.cs.hlsl cs_6_9 main SHADER_OUTPUTS
    OUTPUT_NAME RadianceCacheUpdateLegacy -D RCU_COMPACT_DISPATCH=0)
# ReGIR — pools de luz em grade de mundo para os hits secundarios.
smile_compile_shader(GI/ReGIRBuild.cs.hlsl   cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(GI/ReGIRAverage.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
# Pack dos inputs do NRD (inclui NRD.hlsli p/ o encode R10G10B10A2 que casa com o decode interno)
smile_compile_shader(GI/ReSTIRNrdPack.cs.hlsl   cs_6_6 main SHADER_OUTPUTS ${SMILE_NRD_SHADER_INCLUDE})
# Debug do DDGI (passe grafico separado): esferas de probe + wireframe do volume + stats
smile_compile_shader(GI/DDGIDebugProbes.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugProbes.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugVolume.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugVolume.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugRays.vs.hlsl   vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugRays.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugStats.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugPoint.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)

# ReSTIR DI: candidatas uniformes + temporal, depois spatial + um shadow ray final.
smile_compile_shader(Lighting/ReSTIRDIInitialTemporal.cs.hlsl cs_6_9 main SHADER_OUTPUTS)
smile_compile_shader(Lighting/ReSTIRDISpatial.cs.hlsl         cs_6_9 main SHADER_OUTPUTS
    OUTPUT_NAME ReSTIRDISpatialDerived -D SMILE_TEMPORAL_DERIVED=1)
# Instancia RELAX dedicada ao direto: pack demodulado e composite remodulado.
smile_compile_shader(Lighting/ReSTIRDINrdPack.cs.hlsl          cs_6_6 main SHADER_OUTPUTS ${SMILE_NRD_SHADER_INCLUDE})
smile_compile_shader(Lighting/ReSTIRDINrdComposite.cs.hlsl     cs_6_6 main SHADER_OUTPUTS)
# Mesh lights: extrai os triangulos emissivos da cena para o pool de luzes.
smile_compile_shader(Lighting/MeshLightExtract.cs.hlsl         cs_6_6 main SHADER_OUTPUTS)

# Specular GI (reflexoes RT, estilo Lumen) — trace (DXR 1.2 inline + OMM, SM 6.9)
smile_compile_shader(Reflections/ReflectionTrace.cs.hlsl       cs_6_9 main SHADER_OUTPUTS
    OUTPUT_NAME ReflectionTraceDerived -D SMILE_TEMPORAL_DERIVED=1)
if(SMILE_SHADER_TIMER_AVAILABLE)
    smile_compile_shader(Reflections/ReflectionTrace.cs.hlsl cs_6_9 main SHADER_OUTPUTS
        OUTPUT_NAME ReflectionTraceTimedDerived ${SMILE_TIMER_FLAGS} -D SMILE_TEMPORAL_DERIVED=1)
endif()
smile_compile_shader(Reflections/ReflectionTraceMirror.cs.hlsl cs_6_9 main SHADER_OUTPUTS
    OUTPUT_NAME ReflectionTraceMirrorDerived -D SMILE_TEMPORAL_DERIVED=1)
smile_compile_shader(Reflections/WaterReflectionTrace.cs.hlsl  cs_6_9 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/WaterReflectionTemporal.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionResolve.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionTemporal.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionSpatial.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionComposite.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/WaterReflectionComposite.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
# Pack do sinal especular p/ o NRD (RELAX) — inclui NRD.hlsli p/ o RELAX_FrontEnd_Pack*
smile_compile_shader(Reflections/ReflectionNrdPack.cs.hlsl   cs_6_0 main SHADER_OUTPUTS ${SMILE_NRD_SHADER_INCLUDE})

# DLSS Ray Reconstruction — guides de material (albedo difuso/especular, normal-roughness) derivados
# do G-buffer + extracao do hitDist especular do Resolved. Alimentam o slEvaluateFeature(kFeatureDLSS_RR).
smile_compile_shader(Upscale/DlssRRGuides.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Upscale/DlssRRSpecHit.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Upscale/DlssRRWaterSpecHit.cs.hlsl cs_6_0 main SHADER_OUTPUTS)

# Motion vector do background (ceu/nuvens/fog) — preenche o velocity ZERO que o G-buffer deixa, senao o
# DLSS/RR/TAA reprojeta o historico do ceu do lugar errado ao girar a camera (ghosting).
smile_compile_shader(Upscale/BackgroundVelocity.cs.hlsl cs_6_0 main SHADER_OUTPUTS)

# Vetores temporais confiaveis (RT Gems II, cap. 25): historico de superficie + vetor dual.
smile_compile_shader(Temporal/TemporalSurface.cs.hlsl cs_6_9 main SHADER_OUTPUTS)
smile_compile_shader(Temporal/DualMotion.cs.hlsl      cs_6_0 main SHADER_OUTPUTS
    OUTPUT_NAME DualMotionDerived -D SMILE_TEMPORAL_DERIVED=1)
# Sonda de teste: expoe TemporalResolvePreviousIndex para execucao em WARP. Nao entra em passe
# nenhum, mas passa pelo MESMO DXC -- testar a fonte sob outro compilador nao provaria o mesmo.
smile_compile_shader(Temporal/TemporalResolveProbe.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
# Sonda de PARIDADE do par derivado (Fase 4): compara a inversa e a ordem do produto do HLSL com as
# do C++. A tabela de 128 B por instancia esta sendo aposentada em favor desta derivacao, e um sinal
# de cofator trocado nao daria erro -- daria reprojecao errada so em movimento.
smile_compile_shader(Temporal/TemporalTransformProbe.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
# Sonda de ABI das tabelas canonicas da cena (Fase 2). Mesma logica da de cima: o static_assert do
# C++ nao diz nada sobre o espelho HLSL, e um campo trocado de lugar compila dos dois lados.
smile_compile_shader(Scene/GpuSceneTablesProbe.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
# Expansao de ranges em lista de slots (Fase 4). Entrada do culling da Fase 6; nesta fase nao
# rejeita ninguem, so valida slot/generation e conta. cs_6_0 porque nao usa heap diretamente
# indexado -- os buffers chegam por descriptor table, como o dispatch de HZB.
smile_compile_shader(GpuScene/ExpandInstanceRanges.cs.hlsl cs_6_0 main SHADER_OUTPUTS)

# AO (GTAO) — oclusao de contato em screen-space (so depth), aplicada no termo indireto
# Occlusion culling — cadeia HZB (min-reduce do depth em reverse-Z, estilo UE)
# + teste de AABB (1 thread/objeto, port do HZBTestPS/NaniteHZBCull)
smile_compile_shader(HZB/HZBBuild.cs.hlsl    cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(HZB/HZBTest.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)

smile_compile_shader(AO/GTAO.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(AO/GTAOBlur.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(AO/GTAOUpsample.cs.hlsl cs_6_0 main SHADER_OUTPUTS)

smile_compile_shader(IBL/EquirectToCube.cs.hlsl        cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(IBL/MipGen.cs.hlsl                cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(IBL/IrradianceConvolution.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(IBL/SpecularPrefilter.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(IBL/BRDFIntegration.cs.hlsl       cs_6_0 main SHADER_OUTPUTS)

# Atmosfera (Hillaire) — LUTs base + Sky-View + render do céu
smile_compile_shader(Atmosphere/BakeTransmittance.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/BakeMultiScatter.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/BakeSkyView.cs.hlsl       cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/SkyAtmosphere.vs.hlsl     vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/SkyAtmosphere.ps.hlsl     ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/BakeAerialPerspective.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/IntegrateSkyAmbient.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/BakeSkyReflection.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/StarField.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Atmosphere/StarField.ps.hlsl ps_6_0 main SHADER_OUTPUTS)

# Fog deferido (UE5): aerial perspective froxel + exponential height fog
smile_compile_shader(Fog/FogFullscreen.vs.hlsl            vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Fog/FogFullscreen.ps.hlsl            ps_6_0 main SHADER_OUTPUTS)
# Froxel volumetric fog (UE): densidade -> scattering (sol CSM + ambiente DDGI) -> integracao
smile_compile_shader(Fog/VolumetricFogSetup.cs.hlsl       cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Fog/VolumetricFogScattering.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Fog/VolumetricFogIntegrate.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Fog/VolumetricFogConsDepth.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)

# Chuva — F1 wetness deferred no G-buffer + F3 cortina de gotas (estilo CRainStage/SceneRain)
smile_compile_shader(Rain/RainWetness.ps.hlsl             ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Rain/RainCurtain.ps.hlsl             ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Rain/RainParticles.vs.hlsl           vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Rain/RainParticles.ps.hlsl           ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Rain/RainSplash.vs.hlsl              vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Rain/RainSplash.ps.hlsl              ps_6_0 main SHADER_OUTPUTS)

# Sun shafts volumetricos — raymarch CSM meia-res + acumulacao temporal (fog consome via t2)
smile_compile_shader(SunShafts/SunShaftsVolumetric.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(SunShafts/SunShaftsTemporal.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)

# Nuvens volumetricas — bake de ruido 3D + raymarch + composite
smile_compile_shader(Clouds/BakeBaseNoise.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/BakeDetailNoise.cs.hlsl       cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/BakeWeather.cs.hlsl           cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudRaymarch.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudTemporal.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudShadowMap.cs.hlsl        cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudComposite.vs.hlsl        vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudComposite.ps.hlsl        ps_6_0 main SHADER_OUTPUTS)

# Terreno F1 — heightmap + morph continuo de LOD (Flax/CDLOD) + LOD por screen-size (UE)
smile_compile_shader(Terrain/Terrain.vs.hlsl              vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Terrain/TerrainShadow.vs.hlsl        vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Terrain/TerrainGBuffer.ps.hlsl       ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Terrain/TerrainDepthNormal.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)

# Oceano (port fiel da CryEngine) — superficie (projected grid + shading)
smile_compile_shader(Water/WaterSurface.vs.hlsl           vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/WaterSurface.ps.hlsl           ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/WaterSurface.ps.hlsl           ps_6_0 mainBase SHADER_OUTPUTS
                     OUTPUT_NAME WaterSurfaceBase)
smile_compile_shader(Water/WaterSurface.ps.hlsl           ps_6_0 mainMasks SHADER_OUTPUTS
                     OUTPUT_NAME WaterSurfaceMasks)
smile_compile_shader(Water/WaterSurface.ps.hlsl           ps_6_0 mainReflection SHADER_OUTPUTS
                     OUTPUT_NAME WaterSurfaceReflection)
smile_compile_shader(Water/WaterGenerateDraws.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
# Oceano FFT na GPU (256^2) — espectro -> FFT -> deslocamento -> gradientes/normal
smile_compile_shader(Water/OceanUpdateSpectrum.cs.hlsl    cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanFFT.cs.hlsl               cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanCreateDisplacement.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanGradients.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanDisplacementMip.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanNormalMip.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/WaterSceneColorMip.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)

# Pos-processamento e Tonemapping
smile_compile_shader(Tonemap/PostProcess.vs.hlsl          vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/BloomExtract.ps.hlsl         ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/BloomBlur.ps.hlsl            ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/BloomDownsample.ps.hlsl      ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/BloomUpsample.ps.hlsl        ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/FinalTonemap.ps.hlsl         ps_6_0 main SHADER_OUTPUTS)

# TAA — resolve temporal (reprojecao so-camera + Karis tonemap-weighted blend)
smile_compile_shader(Tonemap/TAAResolve.ps.hlsl           ps_6_0 main SHADER_OUTPUTS)
# Debug — heatmap de flicker temporal (variancia por pixel)
smile_compile_shader(Tonemap/FlickerHeatmap.ps.hlsl       ps_6_0 main SHADER_OUTPUTS)

# Selecao no editor — picking (GPU ID-buffer, estilo hit-proxy) + outline (mascara + edge-detect)
smile_compile_shader(Selection/SelectionGeom.vs.hlsl      vs_6_0 main SHADER_OUTPUTS)
# Permutacao de transform da Fase 4, usada SO pelo picking. O contorno de selecao continua no
# .cso classico acima: ele tem ObjectCB proprio e nao le a tabela.
smile_compile_shader(Selection/SelectionGeom.vs.hlsl vs_6_6 main SHADER_OUTPUTS
    OUTPUT_NAME SelectionGeomVsBindless ${SMILE_INSTANCE_BINDLESS_FLAGS})
smile_compile_shader(Selection/ObjectID.ps.hlsl           ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Selection/ObjectIDAlpha.ps.hlsl      ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Selection/SelectionMask.ps.hlsl      ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Selection/SelectionMaskAlpha.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Selection/SelectionOutline.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)

# DebugDraw: servico de desenho 3D imediato p/ tooling do editor (gizmo etc.)
smile_compile_shader(Gizmo/DebugDraw.vs.hlsl              vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/DebugDraw.ps.hlsl              ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/DebugDrawOccluded.ps.hlsl      ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/DebugDrawLine.vs.hlsl          vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/DebugDrawLine.ps.hlsl          ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/DebugDrawLineDepth.ps.hlsl     ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/LightIcon.vs.hlsl              vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/LightIcon.ps.hlsl              ps_6_0 main SHADER_OUTPUTS)


# Lista do IDE derivada, nao mantida a mao: todos os .hlsl registrados acima + todos os .hlsli
# da arvore. A lista manual anterior tinha ~150 linhas e ja divergira em 14 shaders e 9 headers.
smile_shader_target_sources(SHADER_IDE_SOURCES)

add_custom_target(Shaders
    DEPENDS ${SHADER_OUTPUTS}
    SOURCES ${SHADER_IDE_SOURCES}
)

# Mantem a pasta Shaders visivel no Visual Studio com seus .hlsl
set_target_properties(Shaders PROPERTIES FOLDER "Engine/Assets")
