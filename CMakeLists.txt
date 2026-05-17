cmake_minimum_required(VERSION 3.25)

# Projeto base SmileEngine - Qt 6 + DirectX 12
project(SmileEngine LANGUAGES CXX)

# Versao do projeto - usado pelo VersionInfo.h gerado e pela tela About
set(SMILE_VERSION_MAJOR 0)
set(SMILE_VERSION_MINOR 1)
set(SMILE_VERSION_PATCH 0)
string(TIMESTAMP SMILE_BUILD_DATE   "%Y-%m-%d %H:%M" UTC)
string(TIMESTAMP SMILE_BUILD_NUMBER "%Y%m%d")

# Padrao C++ moderno
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Forca x64 (DirectX 12 e Qt 6 sao x64)
if(NOT CMAKE_GENERATOR_PLATFORM)
    set(CMAKE_GENERATOR_PLATFORM x64 CACHE STRING "" FORCE)
endif()

# Saida unificada para facilitar deploy de DLLs do Qt junto do executavel
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Diretorio onde os .cso compilados ficam, exposto para o codigo C++
set(SMILE_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/Shaders" CACHE PATH "Diretorio de saida dos shaders compilados")
file(MAKE_DIRECTORY ${SMILE_SHADER_OUTPUT_DIR})

# Default Qt prefix path (kit MSVC 2022 64 bits) - pode ser sobrescrito via -D
if(NOT DEFINED CMAKE_PREFIX_PATH OR CMAKE_PREFIX_PATH STREQUAL "")
    set(CMAKE_PREFIX_PATH "C:/Qt/6.10.2/msvc2022_64" CACHE PATH "Prefixo do Qt" FORCE)
endif()

# Flags MSVC mais rigorosas
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /MP /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# Localiza Qt 6
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

# Modulo cmake interno: utilitario para compilar shaders HLSL
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompileShaders)

# Subdiretorios
add_subdirectory(Engine)
add_subdirectory(Shaders)
add_subdirectory(Editor)
