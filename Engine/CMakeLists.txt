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
    Include/Smile/Graphics/ReGIR.h
    Include/Smile/Graphics/ReSTIRDI.h
    Include/Smile/Graphics/NrdDenoiser.h
    Include/Smile/Graphics/AmbientOcclusion.h
    Include/Smile/Graphics/HiZOcclusion.h
    Include/Smile/Graphics/ShaderUtils.h
    Include/Smile/Graphics/PostProcess.h
    Include/Smile/Graphics/Upscaler.h
    Include/Smile/Graphics/FsrPass.h
    Include/Smile/Graphics/DlssPass.h
    Include/Smile/Graphics/DlssRRPass.h
    Include/Smile/Graphics/DlssRRGuides.h
    Include/Smile/Graphics/BackgroundVelocity.h
    Include/Smile/Graphics/DebugTargets.h
    Include/Smile/Graphics/DebugView.h
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
    Source/Graphics/ComputeQueue.cpp
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
    Source/Graphics/Terrain.cpp
    Source/Graphics/ComputePipeline.cpp
    Source/Graphics/VolumetricPipeline.cpp
    Source/Graphics/HDREnvironment.cpp
    Source/Graphics/Atmosphere.cpp
    Source/Graphics/CloudNoise.cpp
    Source/Graphics/VolumetricClouds.cpp
    Source/Graphics/Skybox.cpp
    Source/Graphics/Fog.cpp
    Source/Graphics/VolumetricFog.cpp
    Source/Graphics/SunShafts.cpp
    Source/Graphics/RainWetness.cpp
    Source/Graphics/SunShadows.cpp
    Source/Graphics/LocalShadows.cpp
    Source/Graphics/RaytracingScene.cpp
    Source/Graphics/DDGI.cpp
    Source/Graphics/DDGIDebug.cpp
    Source/Graphics/ReSTIRGI.cpp
    Source/Graphics/ReGIR.cpp
    Source/Graphics/ReSTIRDI.cpp
    Source/Graphics/NrdDenoiser.cpp
    Source/Graphics/Reflections.cpp
    Source/Graphics/AmbientOcclusion.cpp
    Source/Graphics/HiZOcclusion.cpp
    Source/Graphics/ShaderUtils.cpp
    Source/Graphics/PostProcess.cpp
    Source/Graphics/TemporalAA.cpp
    Source/Graphics/FsrPass.cpp
    Source/Graphics/DlssPass.cpp
    Source/Graphics/DlssRRPass.cpp
    Source/Graphics/DlssRRGuides.cpp
    Source/Graphics/BackgroundVelocity.cpp
    Source/Graphics/DebugTargets.cpp
    Source/Graphics/DebugView.cpp
    Source/Graphics/FlickerHeatmap.cpp
    Source/Graphics/OceanFFT.cpp
    Source/Graphics/Water.cpp
    Source/Graphics/MaterialPreview.cpp
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

# === FSR (AMD FidelityFX Super Resolution — upscaler FSR 3.1 via ffx-api) ========================
# FidelityFX SDK 1.1.4 com a API unificada (ffx-api): linka a import lib do amd_fidelityfx_dx12.dll +
# a DLL fica ao lado do exe (os shaders vem embutidos na DLL). Por ser C-ABI atraves de fronteira de
# DLL, NAO tem conflito de CRT — vale p/ Debug E Release (a integracao anterior era Release-only por causa das
# static libs /MD). Se ausente, FsrPass compila como stub (SMILE_FSR_ENABLED=0) e a engine usa o TAA.
set(SMILE_FSR_ROOT "D:/Engines/FidelityFX-SDK-v1.1.4" CACHE PATH "Raiz do FidelityFX SDK (ffx-api)")
set(_fsr_inc "${SMILE_FSR_ROOT}/ffx-api/include")
set(_fsr_lib "${SMILE_FSR_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_dx12.lib")
set(SMILE_FSR_DLL "${SMILE_FSR_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll"
    CACHE FILEPATH "amd_fidelityfx_dx12.dll p/ copiar ao lado do exe")
if(EXISTS "${_fsr_inc}/ffx_api/ffx_api.h" AND EXISTS "${_fsr_lib}")
    message(STATUS "FSR encontrado: ${SMILE_FSR_ROOT} (ffx-api, Debug+Release)")
    target_include_directories(SmileEngine PRIVATE "${_fsr_inc}")
    target_compile_definitions(SmileEngine PRIVATE SMILE_FSR_ENABLED=1)
    target_link_libraries(SmileEngine PUBLIC "${_fsr_lib}")
else()
    message(WARNING "FSR nao encontrado em ${SMILE_FSR_ROOT} — FsrPass compila como stub")
    target_compile_definitions(SmileEngine PRIVATE SMILE_FSR_ENABLED=0)
endif()

# === Streamline (NVIDIA DLSS Super Resolution — kFeatureDLSS) ====================================
# Manual hooking: linka a import lib sl.interposer + as DLLs (production) ficam ao lado do exe. As
# funcoes core sao extern "C" via DLL, entao vale Debug E Release. Se ausente, DlssPass compila como
# stub (SMILE_SL_ENABLED=0) e o DLSS some do seletor (Available()=false), caindo no FSR/None.
set(SMILE_SL_ROOT "D:/Engines/Streamline" CACHE PATH "Raiz do NVIDIA Streamline SDK")
set(_sl_inc "${SMILE_SL_ROOT}/include")
set(_sl_lib "${SMILE_SL_ROOT}/lib/x64/sl.interposer.lib")
set(_sl_bin "${SMILE_SL_ROOT}/bin/x64")
# DLLs production a copiar ao lado do exe. SR = sl.dlss/nvngx_dlss; RR (Ray Reconstruction) =
# sl.dlss_d/nvngx_dlssd (plugin + modelo neural do denoiser).
set(SMILE_SL_DLLS
    "${_sl_bin}/sl.interposer.dll"
    "${_sl_bin}/sl.common.dll"
    "${_sl_bin}/sl.dlss.dll"
    "${_sl_bin}/nvngx_dlss.dll"
    "${_sl_bin}/sl.dlss_d.dll"
    "${_sl_bin}/nvngx_dlssd.dll"
    CACHE STRING "DLLs do Streamline/DLSS (SR + RR) p/ copiar ao lado do exe")
# Migra caches antigos: set(... CACHE) nao reescreve um cache ja existente, entao um build/ configurado
# antes do RR ficaria sem os 2 DLLs novos. Acrescenta-os (com FORCE) so quando faltarem, preservando
# qualquer customizacao da lista.
foreach(_rr_dll "${_sl_bin}/sl.dlss_d.dll" "${_sl_bin}/nvngx_dlssd.dll")
    if(NOT "${_rr_dll}" IN_LIST SMILE_SL_DLLS)
        list(APPEND SMILE_SL_DLLS "${_rr_dll}")
        set(SMILE_SL_DLLS "${SMILE_SL_DLLS}" CACHE STRING
            "DLLs do Streamline/DLSS (SR + RR) p/ copiar ao lado do exe" FORCE)
    endif()
endforeach()
if(EXISTS "${_sl_inc}/sl.h" AND EXISTS "${_sl_lib}")
    message(STATUS "Streamline/DLSS encontrado: ${SMILE_SL_ROOT}")
    target_include_directories(SmileEngine PRIVATE "${_sl_inc}")
    target_compile_definitions(SmileEngine PRIVATE SMILE_SL_ENABLED=1)
    target_link_libraries(SmileEngine PUBLIC "${_sl_lib}")
else()
    message(WARNING "Streamline nao encontrado em ${SMILE_SL_ROOT} — DlssPass compila como stub")
    target_compile_definitions(SmileEngine PRIVATE SMILE_SL_ENABLED=0)
endif()

# === NRD (NVIDIA Real-time Denoising) — denoiser do ReSTIR GI (RELAX_DIFFUSE) ===================
# Caminho D3D12 DIRETO (sem NRI): linka a import lib + a DLL fica ao lado do exe. NRD usa
# AllocationCallbacks proprios, entao a MESMA DLL Release serve p/ Debug+Release da engine (nao
# tem conflito de CRT como a integracao FSR anterior). Exposto via SMILE_NRD_ROOT. Ver memory/project_restir_gi_roadmap.
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
