# Compilacao dos shaders HLSL via DXC.
# Cria o target "Shaders" como dependencia do Editor para garantir build na ordem certa.

set(SHADER_OUTPUTS "")

smile_compile_shader(Triangle.vs.hlsl vs_6_0 main SHADER_OUTPUTS)

# Deferred shading — geometry pass (preenche o G-buffer MRT) + lighting fullscreen + debug
smile_compile_shader(GBuffer.ps.hlsl          ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GBufferDebug.ps.hlsl     ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(DeferredLighting.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
# Passe forward de translucidos (materiais Blend: vidro etc.), por cima do HDR iluminado
smile_compile_shader(ForwardBlend.ps.hlsl     ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Skybox.vs.hlsl   vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Skybox.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)

# Depth pre-pass com saida de normal geometrica (G-buffer leve p/ o GTAO)
smile_compile_shader(DepthNormal.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
# Variantes masked/two-sided do prepass (alpha-clip, estilo UE masked prepass)
smile_compile_shader(DepthNormalMasked.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(DepthMasked.ps.hlsl       ps_6_0 main SHADER_OUTPUTS)

# Cascaded Shadow Maps (sol direcional) — depth pass das cascatas
smile_compile_shader(Shadow/ShadowDepth.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Shadow/ShadowDepth.ps.hlsl ps_6_0 main SHADER_OUTPUTS)

# GI (DDGI) — trace via DXR 1.1 inline ray tracing (RayQuery, SM 6.5) + update dos probes
smile_compile_shader(GI/DDGITrace.cs.hlsl      cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIUpdate.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIUpdateDist.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIRelocate.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)
# ReSTIR GI — final-gather difuso por pixel (reservoir espaciotemporal) sobre o DDGI (cache)
smile_compile_shader(GI/ReSTIRGITrace.cs.hlsl   cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(GI/ReSTIRGISpatial.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
# Pack dos inputs do NRD (inclui NRD.hlsli p/ o encode R10G10B10A2 que casa com o decode interno)
smile_compile_shader(GI/ReSTIRNrdPack.cs.hlsl   cs_6_6 main SHADER_OUTPUTS -I "D:/Engines/NRD/Shaders")
# Debug do DDGI (passe grafico separado): esferas de probe + wireframe do volume + stats
smile_compile_shader(GI/DDGIDebugProbes.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugProbes.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugVolume.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugVolume.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugRays.vs.hlsl   vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugRays.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(GI/DDGIDebugStats.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)

# Specular GI (reflexoes RT, estilo Lumen) — trace (DXR inline, SM 6.6) + resolve + composite
smile_compile_shader(Reflections/ReflectionTrace.cs.hlsl       cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionTraceMirror.cs.hlsl cs_6_6 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionResolve.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionTemporal.cs.hlsl  cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionSpatial.cs.hlsl   cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Reflections/ReflectionComposite.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
# Pack do sinal especular p/ o NRD (REBLUR) — inclui NRD.hlsli p/ o packing de hitDist normalizado
smile_compile_shader(Reflections/ReflectionNrdPack.cs.hlsl   cs_6_0 main SHADER_OUTPUTS -I "D:/Engines/NRD/Shaders")

# AO (GTAO) — oclusao de contato em screen-space (so depth), aplicada no termo indireto
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

# Oceano (port fiel da CryEngine) — superficie (projected grid + shading)
smile_compile_shader(Water/WaterSurface.vs.hlsl           vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/WaterSurface.ps.hlsl           ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/WaterGenerateDraws.cs.hlsl     cs_6_0 main SHADER_OUTPUTS)
# Oceano FFT na GPU (256^2) — espectro -> FFT -> deslocamento -> gradientes/normal
smile_compile_shader(Water/OceanUpdateSpectrum.cs.hlsl    cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanFFT.cs.hlsl               cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanCreateDisplacement.cs.hlsl cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanGradients.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanNormalMip.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)

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
smile_compile_shader(Gizmo/LightIcon.vs.hlsl              vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Gizmo/LightIcon.ps.hlsl              ps_6_0 main SHADER_OUTPUTS)


add_custom_target(Shaders
    DEPENDS ${SHADER_OUTPUTS}
    SOURCES
        Common/DepthConfig.hlsli
        GBuffer.hlsli
        GBuffer.ps.hlsl
        GBufferDebug.ps.hlsl
        DeferredLighting.ps.hlsl
        BRDF.hlsli
        Triangle.vs.hlsl
        DepthNormal.ps.hlsl
        Skybox.vs.hlsl
        Skybox.ps.hlsl
        Shadow/CSMCommon.hlsli
        Shadow/ShadowDepth.vs.hlsl
        Shadow/ShadowDepth.ps.hlsl
        GI/DDGICommon.hlsli
        GI/HitShading.hlsli
        GI/DDGITrace.cs.hlsl
        GI/DDGIUpdate.cs.hlsl
        GI/DDGIUpdateDist.cs.hlsl
        GI/DDGIRelocate.cs.hlsl
        GI/DDGIDebugProbes.vs.hlsl
        GI/DDGIDebugProbes.ps.hlsl
        GI/DDGIDebugVolume.vs.hlsl
        GI/DDGIDebugVolume.ps.hlsl
        GI/DDGIDebugRays.vs.hlsl
        GI/DDGIDebugRays.ps.hlsl
        GI/DDGIDebugStats.cs.hlsl
        Reflections/GGXSample.hlsli
        Reflections/ReflectionTrace.cs.hlsl
        Reflections/ReflectionTraceMirror.cs.hlsl
        Reflections/ReflectionResolve.cs.hlsl
        Reflections/ReflectionTemporal.cs.hlsl
        Reflections/ReflectionSpatial.cs.hlsl
        Reflections/ReflectionComposite.ps.hlsl
        Reflections/ReflectionNrdPack.cs.hlsl
        AO/GTAOCommon.hlsli
        AO/GTAO.cs.hlsl
        AO/GTAOBlur.cs.hlsl
        IBL/Common.hlsli
        IBL/EquirectToCube.cs.hlsl
        IBL/MipGen.cs.hlsl
        IBL/IrradianceConvolution.cs.hlsl
        IBL/SpecularPrefilter.cs.hlsl
        IBL/BRDFIntegration.cs.hlsl
        Atmosphere/AtmosphereCommon.hlsli
        Atmosphere/BakeTransmittance.cs.hlsl
        Atmosphere/BakeMultiScatter.cs.hlsl
        Atmosphere/BakeSkyView.cs.hlsl
        Atmosphere/SkyAtmosphere.vs.hlsl
        Atmosphere/SkyAtmosphere.ps.hlsl
        Atmosphere/BakeAerialPerspective.cs.hlsl
        Atmosphere/IntegrateSkyAmbient.cs.hlsl
        Atmosphere/BakeSkyReflection.cs.hlsl
        Atmosphere/StarField.vs.hlsl
        Atmosphere/StarField.ps.hlsl
        Fog/FogCommon.hlsli
        Fog/FogApply.hlsli
        Fog/FogFullscreen.vs.hlsl
        Fog/FogFullscreen.ps.hlsl
        Rain/RainCommon.hlsli
        Rain/RainWetness.ps.hlsl
        Rain/RainCurtain.ps.hlsl
        Rain/RainParticles.vs.hlsl
        Rain/RainParticles.ps.hlsl
        Rain/RainSplash.vs.hlsl
        Rain/RainSplash.ps.hlsl
        SunShafts/SunShaftsVolumetric.ps.hlsl
        SunShafts/SunShaftsTemporal.ps.hlsl
        Clouds/CloudNoiseCommon.hlsli
        Clouds/CloudDensity.hlsli
        Clouds/CloudLighting.hlsli
        Clouds/BakeBaseNoise.cs.hlsl
        Clouds/BakeDetailNoise.cs.hlsl
        Clouds/BakeWeather.cs.hlsl
        Clouds/CloudRaymarch.cs.hlsl
        Clouds/CloudTemporal.cs.hlsl
        Clouds/CloudShadowMap.cs.hlsl
        Clouds/CloudComposite.vs.hlsl
        Clouds/CloudComposite.ps.hlsl
        Water/WaterCommon.hlsli
        Water/WaterSurface.vs.hlsl
        Water/WaterSurface.ps.hlsl
        Water/WaterGenerateDraws.cs.hlsl
        Water/OceanFFTCommon.hlsli
        Water/OceanUpdateSpectrum.cs.hlsl
        Water/OceanFFT.cs.hlsl
        Water/OceanCreateDisplacement.cs.hlsl
        Water/OceanGradients.cs.hlsl
        Water/OceanNormalMip.cs.hlsl
        Tonemap/PostProcess.vs.hlsl
        Tonemap/BloomExtract.ps.hlsl
        Tonemap/BloomBlur.ps.hlsl
        Tonemap/BloomDownsample.ps.hlsl
        Tonemap/BloomUpsample.ps.hlsl
        Tonemap/FinalTonemap.ps.hlsl
        Tonemap/TAAResolve.ps.hlsl
        Tonemap/FlickerHeatmap.ps.hlsl
        Selection/SelectionGeom.vs.hlsl
        Selection/ObjectID.ps.hlsl
        Selection/SelectionMask.ps.hlsl
        Selection/SelectionOutline.ps.hlsl
        Gizmo/DebugDraw.vs.hlsl
        Gizmo/DebugDraw.ps.hlsl
        Gizmo/DebugDrawOccluded.ps.hlsl
        Gizmo/LightIcon.vs.hlsl
        Gizmo/LightIcon.ps.hlsl
)

# Mantem a pasta Shaders visivel no Visual Studio com seus .hlsl
set_target_properties(Shaders PROPERTIES FOLDER "Build")
