# Compilacao dos shaders HLSL via DXC.
# Cria o target "Shaders" como dependencia do Editor para garantir build na ordem certa.

set(SHADER_OUTPUTS "")

smile_compile_shader(Triangle.vs.hlsl vs_6_0 main SHADER_OUTPUTS)
smile_compile_shader(Triangle.ps.hlsl ps_6_0 main SHADER_OUTPUTS)

add_custom_target(Shaders
    DEPENDS ${SHADER_OUTPUTS}
    SOURCES
        Triangle.vs.hlsl
        Triangle.ps.hlsl
)

# Mantem a pasta Shaders visivel no Visual Studio com seus .hlsl
set_target_properties(Shaders PROPERTIES FOLDER "Build")
