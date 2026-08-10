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

# Deferred shading — geometry pass (preenche o G-buffer MRT) + lighting fullscreen + debug
smile_compile_shader(GBuffer.ps.hlsl          ps_6_0 main SHADER_OUTPUTS)
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

# Cascaded Shadow Maps (sol direcional) — depth pass das cascatas
smile_compile_shader(Shadow/ShadowDepth.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Shadow/ShadowDepth.ps.hlsl ps_6_0 main SHADER_OUTPUTS)

# GI (DDGI) — trace via DXR 1.1 inline ray tracing (RayQuery, SM 6.5) + update dos probes
# Debug da BVH: raio primario por pixel na TLAS (GPU Zen 3, 7.3.3). cs_6_6 pelo RayQuery + heap
# diretamente indexado (VB/IB bindless do alpha-test e da normal do hit).
smile_compile_shader(Debug/BvhDebug.cs.hlsl    cs_6_6 main SHADER_OUTPUTS)

smile_compile_shader(GI/DDGITrace.cs.hlsl      cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIUpdate.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIUpdateDist.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIRelocate.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)
# ReSTIR GI — final-gather difuso por pixel (reservoir espaciotemporal) sobre o DDGI (cache)
smile_compile_shader(GI/ReSTIRGITrace.cs.hlsl   cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(GI/ReSTIRGISpatial.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
if(SMILE_SHADER_TIMER_AVAILABLE)
    smile_compile_shader(GI/ReSTIRGITrace.cs.hlsl cs_6_6 main SHADER_OUTPUTS
        OUTPUT_NAME ReSTIRGITraceTimed ${SMILE_TIMER_FLAGS})
endif()
# World radiance cache — hash espacial de radiancia de saida, terminador dos raios secundarios.
# O update e a query moram no ShadeSurfaceHit (RadianceCache.hlsli, sem .cso proprio); so o
# resolve e um passe.
# cs_6_6 e nao 6_0: o resolve so usa u0/u1/u2 explicitos, mas inclui o RadianceCache.hlsli, e as
# funcoes de query/update de la acessam ResourceDescriptorHeap (bindless, SM 6.6). Os cinco
# traces que consomem o cache ja sao 6_6.
smile_compile_shader(GI/RadianceCacheResolve.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(GI/RadianceCacheDebug.cs.hlsl   cs_6_6 main SHADER_OUTPUTS)
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
smile_compile_shader(Lighting/ReSTIRDIInitialTemporal.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(Lighting/ReSTIRDISpatial.cs.hlsl         cs_6_6 main SHADER_OUTPUTS)
# Instancia RELAX dedicada ao direto: pack demodulado e composite remodulado.
smile_compile_shader(Lighting/ReSTIRDINrdPack.cs.hlsl          cs_6_6 main SHADER_OUTPUTS ${SMILE_NRD_SHADER_INCLUDE})
smile_compile_shader(Lighting/ReSTIRDINrdComposite.cs.hlsl     cs_6_6 main SHADER_OUTPUTS)
# Mesh lights: extrai os triangulos emissivos da cena para o pool de luzes.
smile_compile_shader(Lighting/MeshLightExtract.cs.hlsl         cs_6_6 main SHADER_OUTPUTS)

# Specular GI (reflexoes RT, estilo Lumen) — trace (DXR inline, SM 6.6) + resolve + composite
smile_compile_shader(Reflections/ReflectionTrace.cs.hlsl       cs_6_6 main SHADER_OUTPUTS)
if(SMILE_SHADER_TIMER_AVAILABLE)
    smile_compile_shader(Reflections/ReflectionTrace.cs.hlsl cs_6_6 main SHADER_OUTPUTS
        OUTPUT_NAME ReflectionTraceTimed ${SMILE_TIMER_FLAGS})
endif()
smile_compile_shader(Reflections/ReflectionTraceMirror.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/WaterReflectionTrace.cs.hlsl  cs_6_6 main SHADER_OUTPUTS)
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
smile_compile_shader(Temporal/TemporalSurface.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(Temporal/DualMotion.cs.hlsl      cs_6_0 main SHADER_OUTPUTS)

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
smile_compile_shader(Selection/ObjectID.ps.hlsl           ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Selection/SelectionMask.ps.hlsl      ps_6_0 main SHADER_OUTPUTS)
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
set_target_properties(Shaders PROPERTIES FOLDER "Build")
