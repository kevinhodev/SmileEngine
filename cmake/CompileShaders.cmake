# CompileShaders.cmake
# Utilitario para compilar shaders HLSL com DXC (DirectX Shader Compiler) em build time.

# Localiza o DXC do Windows SDK 10.0.22621.0
find_program(SMILE_DXC_EXECUTABLE
    NAMES dxc.exe dxc
    PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/x64"
    NO_DEFAULT_PATH
)

if(NOT SMILE_DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC (dxc.exe) nao encontrado. Instale o Windows SDK 10.0.22621.0.")
else()
    message(STATUS "DXC encontrado em: ${SMILE_DXC_EXECUTABLE}")
endif()

# smile_compile_shader: registra um shader HLSL para compilacao
#   HLSL_FILE  - caminho relativo ao CMAKE_CURRENT_SOURCE_DIR do .hlsl
#   PROFILE    - perfil DXC, ex.: vs_6_0, ps_6_0
#   ENTRY      - funcao de entrada (geralmente "main")
#   OUT_VAR    - nome de variavel onde o caminho do .cso gerado eh anexado
function(smile_compile_shader HLSL_FILE PROFILE ENTRY OUT_VAR)
    get_filename_component(SHADER_NAME ${HLSL_FILE} NAME_WE)
    set(SHADER_INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/${HLSL_FILE}")
    set(SHADER_OUTPUT "${SMILE_SHADER_OUTPUT_DIR}/${SHADER_NAME}.${PROFILE}.cso")

    # Flags: debug em Debug, otimizacao em Release
    set(DXC_FLAGS -T ${PROFILE} -E ${ENTRY} -Fo ${SHADER_OUTPUT})
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_CONFIGURATION_TYPES)
        # Multi-config (VS): adiciona -Zi/-Od via expressao de geracao
        list(APPEND DXC_FLAGS
            $<$<CONFIG:Debug>:-Zi>
            $<$<CONFIG:Debug>:-Od>
            $<$<CONFIG:Debug>:-Qembed_debug>
            $<$<NOT:$<CONFIG:Debug>>:-O3>
        )
    endif()

    add_custom_command(
        OUTPUT  ${SHADER_OUTPUT}
        COMMAND ${SMILE_DXC_EXECUTABLE} ${DXC_FLAGS} ${SHADER_INPUT}
        MAIN_DEPENDENCY ${SHADER_INPUT}
        COMMENT "[DXC] ${SHADER_NAME}.hlsl -> ${SHADER_NAME}.${PROFILE}.cso (${PROFILE})"
        VERBATIM
    )

    # Anexa ao acumulador de saidas para que o target Shaders dependa de todos
    set(_acc ${${OUT_VAR}})
    list(APPEND _acc ${SHADER_OUTPUT})
    set(${OUT_VAR} ${_acc} PARENT_SCOPE)
endfunction()
