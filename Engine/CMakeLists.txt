# Biblioteca estatica da engine - SmileEngine

# Gera VersionInfo.h a partir do template (.in) com as macros definidas no CMake raiz
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Include/Smile/Core/VersionInfo.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Smile/Core/VersionInfo.h
    @ONLY
)

set(ENGINE_HEADERS
    Include/Smile/Core/Types.h
    Include/Smile/Core/Logger.h
    Include/Smile/Core/HResultCheck.h
    Include/Smile/Math/Math.h
    Include/Smile/Math/MathUtils.h
    Include/Smile/Math/Vec2.h
    Include/Smile/Math/Vec3.h
    Include/Smile/Math/Vec4.h
    Include/Smile/Math/Mat44.h
    Include/Smile/Input/CameraInput.h
    Include/Smile/Graphics/D3D12Device.h
    Include/Smile/Graphics/CommandQueue.h
    Include/Smile/Graphics/DescriptorHeap.h
    Include/Smile/Graphics/SwapChain.h
    Include/Smile/Graphics/PipelineState.h
    Include/Smile/Graphics/Camera.h
    Include/Smile/Graphics/Mesh.h
    Include/Smile/Graphics/TextureSRVHeap.h
    Include/Smile/Graphics/Texture.h
    Include/Smile/Graphics/Material.h
    Include/Smile/Graphics/Renderer.h
)

# Header gerado pelo configure_file - listado a parte porque vive em CMAKE_BINARY_DIR
set(ENGINE_GENERATED_HEADERS
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Smile/Core/VersionInfo.h
)

set(ENGINE_SOURCES
    Source/Core/Logger.cpp
    Source/Graphics/D3D12Device.cpp
    Source/Graphics/CommandQueue.cpp
    Source/Graphics/DescriptorHeap.cpp
    Source/Graphics/SwapChain.cpp
    Source/Graphics/PipelineState.cpp
    Source/Graphics/Camera.cpp
    Source/Graphics/Mesh.cpp
    Source/Graphics/TextureSRVHeap.cpp
    Source/Graphics/Texture.cpp
    Source/Graphics/Material.cpp
    Source/Graphics/Renderer.cpp
)

add_library(SmileEngine STATIC
    ${ENGINE_HEADERS}
    ${ENGINE_GENERATED_HEADERS}
    ${ENGINE_SOURCES}
)

target_include_directories(SmileEngine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Diretorio dos shaders compilados, exposto como compile definition
target_compile_definitions(SmileEngine
    PUBLIC
        SMILE_SHADER_DIR="${SMILE_SHADER_OUTPUT_DIR}"
)

# Bibliotecas necessarias para DirectX 12 + DXGI
target_link_libraries(SmileEngine
    PUBLIC
        d3d12
        dxgi
        dxguid
        windowscodecs
        ole32
)

# Organizacao no Visual Studio (filters por pasta)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_HEADERS} ${ENGINE_SOURCES})
