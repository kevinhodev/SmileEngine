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
    Include/Smile/Graphics/GpuMesh.h
    Include/Smile/Scene/Scene.h
    Include/Smile/Scene/CookedFormat.h
    Include/Smile/Graphics/TextureSRVHeap.h
    Include/Smile/Graphics/Texture.h
    Include/Smile/Graphics/CubeTexture.h
    Include/Smile/Graphics/VolumeTexture.h
    Include/Smile/Graphics/Material.h
    Include/Smile/Graphics/GBuffer.h
    Include/Smile/Graphics/GBufferDebug.h
    Include/Smile/Graphics/ComputePipeline.h
    Include/Smile/Graphics/VolumetricPipeline.h
    Include/Smile/Graphics/HDREnvironment.h
    Include/Smile/Graphics/Atmosphere.h
    Include/Smile/Graphics/CloudNoise.h
    Include/Smile/Graphics/VolumetricClouds.h
    Include/Smile/Graphics/Skybox.h
    Include/Smile/Graphics/Fog.h
    Include/Smile/Graphics/Weather.h
    Include/Smile/Graphics/RainWetness.h
    Include/Smile/Graphics/SunShadows.h
    Include/Smile/Graphics/RaytracingScene.h
    Include/Smile/Graphics/DDGI.h
    Include/Smile/Graphics/DDGIDebug.h
    Include/Smile/Graphics/ReSTIRGI.h
    Include/Smile/Graphics/NrdDenoiser.h
    Include/Smile/Graphics/AmbientOcclusion.h
    Include/Smile/Graphics/ShaderUtils.h
    Include/Smile/Graphics/PostProcess.h
    Include/Smile/Graphics/Fsr2Pass.h
    Include/Smile/Graphics/OceanFFT.h
    Include/Smile/Graphics/Water.h
    Include/Smile/Graphics/VramTracker.h
    Include/Smile/Graphics/GpuProfiler.h
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
    Source/Graphics/UploadQueue.cpp
    Source/Graphics/DescriptorHeap.cpp
    Source/Graphics/SwapChain.cpp
    Source/Graphics/PipelineState.cpp
    Source/Graphics/Camera.cpp
    Source/Graphics/Mesh.cpp
    Source/Graphics/GpuMesh.cpp
    Source/Scene/Scene.cpp
    Source/Scene/SceneLoader.cpp
    Source/Graphics/TextureSRVHeap.cpp
    Source/Graphics/Texture.cpp
    Source/Graphics/CubeTexture.cpp
    Source/Graphics/VolumeTexture.cpp
    Source/Graphics/Material.cpp
    Source/Graphics/GBuffer.cpp
    Source/Graphics/GBufferDebug.cpp
    Source/Graphics/ComputePipeline.cpp
    Source/Graphics/VolumetricPipeline.cpp
    Source/Graphics/HDREnvironment.cpp
    Source/Graphics/Atmosphere.cpp
    Source/Graphics/CloudNoise.cpp
    Source/Graphics/VolumetricClouds.cpp
    Source/Graphics/Skybox.cpp
    Source/Graphics/Fog.cpp
    Source/Graphics/SunShafts.cpp
    Source/Graphics/RainWetness.cpp
    Source/Graphics/SunShadows.cpp
    Source/Graphics/LocalShadows.cpp
    Source/Graphics/RaytracingScene.cpp
    Source/Graphics/DDGI.cpp
    Source/Graphics/DDGIDebug.cpp
    Source/Graphics/ReSTIRGI.cpp
    Source/Graphics/NrdDenoiser.cpp
    Source/Graphics/Reflections.cpp
    Source/Graphics/AmbientOcclusion.cpp
    Source/Graphics/ShaderUtils.cpp
    Source/Graphics/PostProcess.cpp
    Source/Graphics/TemporalAA.cpp
    Source/Graphics/Fsr2Pass.cpp
    Source/Graphics/FlickerHeatmap.cpp
    Source/Graphics/OceanFFT.cpp
    Source/Graphics/Water.cpp
    Source/Graphics/Picking.cpp
    Source/Graphics/SelectionOutline.cpp
    Source/Graphics/DebugDraw.cpp
    Source/Graphics/VramTracker.cpp
    Source/Graphics/GpuProfiler.cpp
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
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty
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

# === FSR2 (AMD FidelityFX Super Resolution 2) — upscaler/AA temporal ============================
# SDK buildado externamente (static libs, Release /MD). Linkado SO em config != Debug para nao
# conflitar com o CRT /MDd do Debug; em Debug o Fsr2Pass compila como stub (SMILE_FSR2_ENABLED=0).
# Os shaders (DXIL das permutacoes) ja vem embutidos no lib dx12 — nada em runtime.
# Ver memory/ref-fsr2-integration.md.
set(SMILE_FSR2_ROOT "D:/Engines/FidelityFX-FSR2" CACHE PATH "Raiz do SDK FidelityFX-FSR2")
set(_fsr2_inc    "${SMILE_FSR2_ROOT}/src/ffx-fsr2-api")
set(_fsr2_libdir "${SMILE_FSR2_ROOT}/bin/ffx_fsr2_api")
if(EXISTS "${_fsr2_inc}/ffx_fsr2.h" AND EXISTS "${_fsr2_libdir}/ffx_fsr2_api_dx12_x64.lib")
    message(STATUS "FSR2 encontrado: ${SMILE_FSR2_ROOT} (ativo apenas em Release)")
    target_include_directories(SmileEngine PRIVATE "${_fsr2_inc}")
    target_compile_definitions(SmileEngine PRIVATE SMILE_FSR2_ENABLED=$<IF:$<CONFIG:Debug>,0,1>)
    target_link_libraries(SmileEngine PUBLIC
        $<$<NOT:$<CONFIG:Debug>>:${_fsr2_libdir}/ffx_fsr2_api_x64.lib>
        $<$<NOT:$<CONFIG:Debug>>:${_fsr2_libdir}/ffx_fsr2_api_dx12_x64.lib>)
else()
    message(WARNING "FSR2 nao encontrado em ${SMILE_FSR2_ROOT} — Fsr2Pass compila como stub")
    target_compile_definitions(SmileEngine PRIVATE SMILE_FSR2_ENABLED=0)
endif()

# === NRD (NVIDIA Real-time Denoising) — denoiser do ReSTIR GI (RELAX_DIFFUSE) ===================
# Caminho D3D12 DIRETO (sem NRI): linka a import lib + a DLL fica ao lado do exe. NRD usa
# AllocationCallbacks proprios, entao a MESMA DLL Release serve p/ Debug+Release da engine (nao
# tem conflito de CRT como o FSR2). Exposto via SMILE_NRD_ROOT. Ver memory/project_restir_gi_roadmap.
set(SMILE_NRD_ROOT "D:/Engines/NRD" CACHE PATH "Raiz do NRD SDK")
set(_nrd_inc "${SMILE_NRD_ROOT}/Include")
set(_nrd_lib "${SMILE_NRD_ROOT}/_Bin/Release/NRD.lib")
set(SMILE_NRD_DLL "${SMILE_NRD_ROOT}/_Bin/Release/NRD.dll" CACHE FILEPATH "NRD.dll p/ copiar ao lado do exe")
if(EXISTS "${_nrd_inc}/NRD.h" AND EXISTS "${_nrd_lib}")
    message(STATUS "NRD encontrado: ${SMILE_NRD_ROOT}")
    target_include_directories(SmileEngine PRIVATE "${_nrd_inc}")
    target_compile_definitions(SmileEngine PUBLIC SMILE_NRD_ENABLED=1)
    target_link_libraries(SmileEngine PUBLIC "${_nrd_lib}")
else()
    message(WARNING "NRD nao encontrado em ${SMILE_NRD_ROOT} — NrdDenoiser compila como stub")
    target_compile_definitions(SmileEngine PUBLIC SMILE_NRD_ENABLED=0)
endif()

# Organizacao no Visual Studio (filters por pasta)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_HEADERS} ${ENGINE_SOURCES})
