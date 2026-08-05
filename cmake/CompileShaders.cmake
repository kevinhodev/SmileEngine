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

# _smile_shader_deps: dependencias TRANSITIVAS de #include de um shader.
#
# Por que a mao e nao depfile: o DXC 1.6 que vem no Windows SDK 10.0.22621.0 nao tem -MF/-MD
# (a flag so existe em builds posteriores do upstream), entao nao ha .d para o CMake consumir
# via DEPFILE. A alternativa anterior era um GLOB de TODOS os .hlsli como dependencia de TODO
# shader: correta, mas fazia um toque em qualquer header recompilar os 130 .cso.
#
# Resolucao de include: aspas resolvem relativo ao diretorio do arquivo QUE INCLUI (regra do
# DXC/Clang). O que nao resolver dentro da arvore e externo (ex.: NRD.hlsli, que vem por -I) e
# e ignorado de proposito — nao versionamos SDK de terceiro e ele nao muda entre builds.
#
# Varredura iterativa com conjunto de visitados: um ciclo de include (ou um diamante, que e o
# caso comum aqui) nao trava nem duplica.
function(_smile_shader_deps ROOT_FILE OUT_VAR)
    set(_pending "${ROOT_FILE}")
    set(_seen    "")
    set(_result  "")

    while(_pending)
        list(POP_FRONT _pending _current)
        if("${_current}" IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_current}")
        if(NOT EXISTS "${_current}")
            continue()
        endif()

        get_filename_component(_dir "${_current}" DIRECTORY)
        file(STRINGS "${_current}" _include_lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
        foreach(_line IN LISTS _include_lines)
            string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*$" "\\1" _rel "${_line}")
            get_filename_component(_abs "${_dir}/${_rel}" ABSOLUTE)
            if(EXISTS "${_abs}")
                list(APPEND _pending "${_abs}")
                list(APPEND _result  "${_abs}")
            endif()
        endforeach()
    endwhile()

    if(_result)
        list(REMOVE_DUPLICATES _result)
    endif()
    set(${OUT_VAR} "${_result}" PARENT_SCOPE)
endfunction()

# smile_compile_shader: registra um shader HLSL para compilacao
#   HLSL_FILE  - caminho relativo ao CMAKE_CURRENT_SOURCE_DIR do .hlsl
#   PROFILE    - perfil DXC, ex.: vs_6_0, ps_6_0
#   ENTRY      - funcao de entrada (geralmente "main")
#   OUT_VAR    - nome de variavel onde o caminho do .cso gerado eh anexado
#   ARGN       - (opcional) OUTPUT_NAME <nome> p/ permutacoes, e flags extras passadas ao DXC
#                (ex.: -I <dir>, -D MACRO=val)
#
# Alem de registrar a compilacao, acumula o .hlsl na propriedade global SMILE_SHADER_SOURCES.
# E dela que o target Shaders monta a lista do Visual Studio — nao existe segunda lista para
# manter em sincronia (a antiga divergiu em 14 shaders + 9 .hlsli antes de alguem notar).
function(smile_compile_shader HLSL_FILE PROFILE ENTRY OUT_VAR)
    cmake_parse_arguments(SCS "" "OUTPUT_NAME" "" ${ARGN})
    get_filename_component(SHADER_NAME ${HLSL_FILE} NAME_WE)
    if(SCS_OUTPUT_NAME)
        set(SHADER_NAME ${SCS_OUTPUT_NAME})
    endif()
    set(SHADER_INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/${HLSL_FILE}")
    set(SHADER_OUTPUT "${SMILE_SHADER_OUTPUT_DIR}/${SHADER_NAME}.${PROFILE}.cso")

    if(NOT EXISTS "${SHADER_INPUT}")
        message(FATAL_ERROR "smile_compile_shader: '${HLSL_FILE}' nao existe em ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    # So os .hlsli que este shader realmente alcanca pela cadeia de #include.
    _smile_shader_deps("${SHADER_INPUT}" SHADER_INCLUDE_DEPS)

    # O grafo e lido em tempo de configure: se alguem ADICIONA ou REMOVE um #include, o CMake
    # precisa reconfigurar p/ recalcular. Registrar os arquivos como dependencia de configure
    # faz isso sozinho (o GLOB CONFIGURE_DEPENDS do CMakeLists so pega arquivo novo/apagado,
    # nao mudanca de conteudo).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${SHADER_INPUT}" ${SHADER_INCLUDE_DEPS})

    # Registra a fonte p/ a lista do IDE (dedup no fim: permutacoes repetem o mesmo .hlsl).
    set_property(GLOBAL APPEND PROPERTY SMILE_SHADER_SOURCES "${HLSL_FILE}")

    # Flags: debug em Debug, otimizacao em Release. Sobras do parse = flags extras do chamador.
    set(DXC_FLAGS -T ${PROFILE} -E ${ENTRY} -Fo ${SHADER_OUTPUT} ${SCS_UNPARSED_ARGUMENTS})
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
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SMILE_SHADER_OUTPUT_DIR}"
        COMMAND ${SMILE_DXC_EXECUTABLE} ${DXC_FLAGS} ${SHADER_INPUT}
        MAIN_DEPENDENCY ${SHADER_INPUT}
        DEPENDS ${SHADER_INCLUDE_DEPS}
        COMMENT "[DXC] ${SHADER_NAME}.hlsl -> ${SHADER_NAME}.${PROFILE}.cso (${PROFILE})"
        VERBATIM
    )

    # Anexa ao acumulador de saidas para que o target Shaders dependa de todos
    set(_acc ${${OUT_VAR}})
    list(APPEND _acc ${SHADER_OUTPUT})
    set(${OUT_VAR} ${_acc} PARENT_SCOPE)
endfunction()

# smile_shader_target_sources: lista de fontes p/ o target Shaders (visibilidade no VS).
#   = todos os .hlsl registrados via smile_compile_shader + todos os .hlsli da arvore.
# Avisa sobre .hlsl em disco que ninguem registrou — um shader orfao nao compila e o silencio
# some com ele por semanas.
function(smile_shader_target_sources OUT_VAR)
    get_property(_sources GLOBAL PROPERTY SMILE_SHADER_SOURCES)
    if(_sources)
        list(REMOVE_DUPLICATES _sources)
    endif()

    file(GLOB_RECURSE _headers CONFIGURE_DEPENDS
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.hlsli")

    file(GLOB_RECURSE _on_disk CONFIGURE_DEPENDS
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.hlsl")
    foreach(_shader IN LISTS _on_disk)
        if(NOT "${_shader}" IN_LIST _sources)
            message(WARNING "Shader nao registrado (nao sera compilado): ${_shader}")
        endif()
    endforeach()

    list(SORT _sources)
    list(SORT _headers)
    set(${OUT_VAR} ${_sources} ${_headers} PARENT_SCOPE)
endfunction()
