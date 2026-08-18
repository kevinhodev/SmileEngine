# Source manifest and Visual Studio layout for SmileEngine.
#
# Public headers and implementation files remain physically separated, but every
# Graphics domain is mirrored below Include/Smile and Source. The source groups
# intentionally omit that API/implementation split so a component's .h and .cpp
# appear together in Visual Studio.

set(SMILE_ENGINE_FILES "")

function(smile_engine_group group)
    set(group_files "")

    foreach(file IN LISTS ARGN)
        get_filename_component(absolute_file "${file}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

        if(NOT EXISTS "${absolute_file}")
            message(FATAL_ERROR
                "SmileEngine source '${file}' in group '${group}' does not exist")
        endif()

        list(APPEND group_files "${absolute_file}")
    endforeach()

    source_group("${group}" FILES ${group_files})
    set(SMILE_ENGINE_FILES
        ${SMILE_ENGINE_FILES}
        ${group_files}
        PARENT_SCOPE
    )
endfunction()

function(smile_graphics_domain domain)
    set(domain_files "")
    string(REPLACE "/" "\\" domain_group "${domain}")

    foreach(component IN LISTS ARGN)
        set(header "Include/Smile/Graphics/${domain}/${component}.h")
        set(source "Source/Graphics/${domain}/${component}.cpp")
        set(component_found FALSE)

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
            list(APPEND domain_files "${header}")
            set(component_found TRUE)
        endif()

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
            list(APPEND domain_files "${source}")
            set(component_found TRUE)
        endif()

        if(NOT component_found)
            message(FATAL_ERROR
                "Graphics component '${component}' does not exist in domain '${domain}'")
        endif()
    endforeach()

    smile_engine_group("Graphics\\${domain_group}" ${domain_files})
    set(SMILE_ENGINE_FILES ${SMILE_ENGINE_FILES} PARENT_SCOPE)
endfunction()

smile_engine_group("Core"
    Include/Smile/Core/HResultCheck.h
    Include/Smile/Core/Logger.h
    Include/Smile/Core/Types.h
    Include/Smile/Core/VersionInfo.h.in
    Source/Core/Logger.cpp
)

smile_engine_group("Input"
    Include/Smile/Input/CameraInput.h
)

smile_engine_group("Math"
    Include/Smile/Math/Mat44.h
    Include/Smile/Math/Math.h
    Include/Smile/Math/MathUtils.h
    Include/Smile/Math/Vec2.h
    Include/Smile/Math/Vec3.h
    Include/Smile/Math/Vec4.h
)

smile_engine_group("Scene"
    Include/Smile/Scene/CookedFormat.h
    Include/Smile/Scene/Light.h
    Include/Smile/Scene/Scene.h
    Include/Smile/Scene/SceneLoader.h
    Source/Scene/Scene.cpp
    Source/Scene/SceneLoader.cpp
)

smile_graphics_domain(Renderer
    DepthConfig
    FrameContext
    HistoryDomain
    PassContext
    Renderer
    RendererCapture
    RendererCaptureState
    RendererFrame
    RendererFrameState
    RendererPasses
    RendererScene
    RendererSceneImport
    RendererSceneState
    RenderPass
    RenderSettings
    SceneTargets
)

smile_graphics_domain(Backend
    RenderBackend
)

smile_graphics_domain(Backend/D3D12
    Barriers
    CommandQueue
    ComputePipeline
    ComputeQueue
    D3D12Device
    DescriptorHeap
    GpuResources
    PipelineState
    PipelineUtils
    ShaderUtils
    SwapChain
    TextureSRVHeap
    UploadQueue
)

smile_graphics_domain(Resources
    CubeTexture
    GpuMesh
    Material
    Mesh
    Texture
    VolumeTexture
)

smile_graphics_domain(Scene
    Camera
    GBuffer
    HiZOcclusion
    Terrain
)

smile_graphics_domain(Lighting
    LocalShadows
    MeshLights
    ReSTIRDI
    SunShadows
)

smile_graphics_domain(GI
    AmbientOcclusion
    DDGI
    DDGIDebug
    GIFallback
    GIHitSampling
    IndirectPolicy
    NrdDenoiser
    RadianceCache
    Reflections
    ReGIR
    ReSTIRGI
)

smile_graphics_domain(RayTracing
    RayEpsilons
    RaytracingScene
    RTMasks
    RTTriangle
)

smile_graphics_domain(Environment
    Atmosphere
    CloudNoise
    Fog
    HDREnvironment
    RainWetness
    Skybox
    SunShafts
    TimeOfDay
    VolumetricClouds
    VolumetricFog
    Weather
)

smile_graphics_domain(PostProcess
    BackgroundVelocity
    DlssPass
    DlssRRGuides
    DlssRRPass
    FsrPass
    PostProcess
    TemporalAA
    TemporalMotionVectors
    Upscaler
)

smile_graphics_domain(Water
    OceanFFT
    OceanSpectrum
    Water
)

smile_graphics_domain(Editor
    DebugDraw
    MaterialPreview
    Picking
    SelectionOutline
)

smile_graphics_domain(Debug
    BvhDebugView
    DebugTargets
    DebugView
    FlickerHeatmap
    FrameCapture
    GpuProfiler
    ShaderTimer
    VramTracker
)

smile_engine_group("ThirdParty\\D3D12MA"
    ThirdParty/D3D12MA/D3D12MemAlloc.cpp
    ThirdParty/D3D12MA/D3D12MemAlloc.h
    ThirdParty/D3D12MA/D3D12MemAlloc.natvis
    ThirdParty/D3D12MA/LICENSE.txt
)

# Keep the manifest honest. New engine files force an explicit domain choice,
# while stale or duplicated entries fail at configure time with a useful error.
file(GLOB_RECURSE discovered_engine_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/Smile/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/Smile/*.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
)
list(APPEND discovered_engine_files
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/D3D12MA/D3D12MemAlloc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/D3D12MA/D3D12MemAlloc.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/D3D12MA/D3D12MemAlloc.natvis"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/D3D12MA/LICENSE.txt"
)

set(unique_engine_files ${SMILE_ENGINE_FILES})
list(REMOVE_DUPLICATES unique_engine_files)
list(LENGTH SMILE_ENGINE_FILES engine_file_count)
list(LENGTH unique_engine_files unique_engine_file_count)

if(NOT engine_file_count EQUAL unique_engine_file_count)
    message(FATAL_ERROR "SmileEngine source manifest contains duplicate files")
endif()

set(unclassified_engine_files ${discovered_engine_files})
list(REMOVE_ITEM unclassified_engine_files ${unique_engine_files})

set(stale_engine_files ${unique_engine_files})
list(REMOVE_ITEM stale_engine_files ${discovered_engine_files})

if(unclassified_engine_files)
    string(REPLACE ";" "\n  " unclassified_message "${unclassified_engine_files}")
    message(FATAL_ERROR
        "SmileEngine files without a source group:\n  ${unclassified_message}")
endif()

if(stale_engine_files)
    string(REPLACE ";" "\n  " stale_message "${stale_engine_files}")
    message(FATAL_ERROR
        "SmileEngine source manifest contains missing files:\n  ${stale_message}")
endif()
