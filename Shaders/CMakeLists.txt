# Compilacao dos shaders HLSL via DXC.
# Cria o target "Shaders" como dependencia do Editor para garantir build na ordem certa.

set(SHADER_OUTPUTS "")

smile_compile_shader(Triangle.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Triangle.ps.hlsl ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Skybox.vs.hlsl   vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Skybox.ps.hlsl   ps_6_0 main SHADER_OUTPUTS)

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

# Nuvens volumetricas — bake de ruido 3D + raymarch + composite
smile_compile_shader(Clouds/BakeBaseNoise.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/BakeDetailNoise.cs.hlsl       cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/BakeWeather.cs.hlsl           cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudRaymarch.cs.hlsl         cs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudComposite.vs.hlsl        vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Clouds/CloudComposite.ps.hlsl        ps_6_0 main SHADER_OUTPUTS)

# Agua / oceano FFT
smile_compile_shader(Water/OceanWater.vs.hlsl             vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Water/OceanWater.ps.hlsl             ps_6_0 main SHADER_OUTPUTS)

# Pos-processamento e Tonemapping
smile_compile_shader(Tonemap/PostProcess.vs.hlsl          vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/BloomExtract.ps.hlsl         ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/BloomBlur.ps.hlsl            ps_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Tonemap/FinalTonemap.ps.hlsl         ps_6_0 main SHADER_OUTPUTS)


add_custom_target(Shaders
    DEPENDS ${SHADER_OUTPUTS}
    SOURCES
        Triangle.vs.hlsl
        Triangle.ps.hlsl
        Skybox.vs.hlsl
        Skybox.ps.hlsl
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
        Clouds/CloudNoiseCommon.hlsli
        Clouds/CloudDensity.hlsli
        Clouds/CloudLighting.hlsli
        Clouds/BakeBaseNoise.cs.hlsl
        Clouds/BakeDetailNoise.cs.hlsl
        Clouds/BakeWeather.cs.hlsl
        Clouds/CloudRaymarch.cs.hlsl
        Clouds/CloudComposite.vs.hlsl
        Clouds/CloudComposite.ps.hlsl
        Water/WaterCommon.hlsli
        Water/OceanWater.vs.hlsl
        Water/OceanWater.ps.hlsl
        Tonemap/PostProcess.vs.hlsl
        Tonemap/BloomExtract.ps.hlsl
        Tonemap/BloomBlur.ps.hlsl
        Tonemap/FinalTonemap.ps.hlsl
)

# Mantem a pasta Shaders visivel no Visual Studio com seus .hlsl
set_target_properties(Shaders PROPERTIES FOLDER "Build")
