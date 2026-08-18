add_executable(SmileOceanMathBaselineTests
    OceanMathBaselineTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Water/OceanSpectrum.cpp
)

target_compile_features(SmileOceanMathBaselineTests PRIVATE cxx_std_20)
target_include_directories(SmileOceanMathBaselineTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
set_target_properties(SmileOceanMathBaselineTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.OceanMathBaseline
    COMMAND SmileOceanMathBaselineTests
)

set_tests_properties(Smile.OceanMathBaseline PROPERTIES
    LABELS "ocean;math;baseline"
)

add_executable(SmileTimeOfDayMoonTests
    TimeOfDayMoonTests.cpp
)

target_compile_features(SmileTimeOfDayMoonTests PRIVATE cxx_std_20)
target_include_directories(SmileTimeOfDayMoonTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
set_target_properties(SmileTimeOfDayMoonTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.TimeOfDayMoon
    COMMAND SmileTimeOfDayMoonTests
)

set_tests_properties(Smile.TimeOfDayMoon PROPERTIES
    LABELS "time-of-day;moon;stars;sidereal;math"
)

# Identidade estavel da FScene. Nao usa GPU, mas linka o Scene.cpp, e ele arrasta os dois
# caminhos de upload de malha (que o teste nao chama) — dai GpuMesh/UploadQueue/VramTracker/
# GpuResources e o d3d12.lib apenas para resolver simbolos.
add_executable(SmileSceneIdentityTests
    SceneIdentityTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Scene/Scene.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Resources/GpuMesh.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Resources/Material.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Backend/D3D12/TextureSRVHeap.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Backend/D3D12/GpuResources.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Backend/D3D12/UploadQueue.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Debug/VramTracker.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/Engine/ThirdParty/D3D12MA/D3D12MemAlloc.cpp
)

target_compile_features(SmileSceneIdentityTests PRIVATE cxx_std_20)
target_include_directories(SmileSceneIdentityTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
    ${PROJECT_SOURCE_DIR}/Engine/ThirdParty
)
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/Engine/ThirdParty/D3D12MA/D3D12MemAlloc.cpp
    PROPERTIES COMPILE_OPTIONS "/W0"
)
target_link_libraries(SmileSceneIdentityTests PRIVATE d3d12 dxgi)
set_target_properties(SmileSceneIdentityTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.SceneIdentity
    COMMAND SmileSceneIdentityTests
)

set_tests_properties(Smile.SceneIdentity PROPERTIES
    LABELS "scene;identity;editor"
)

add_executable(SmileRenderPassRegistryTests
    RenderPassRegistryTests.cpp
)

target_compile_features(SmileRenderPassRegistryTests PRIVATE cxx_std_20)
target_link_libraries(SmileRenderPassRegistryTests PRIVATE SmileEngine)
set_target_properties(SmileRenderPassRegistryTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.RenderPassRegistry
    COMMAND SmileRenderPassRegistryTests
)

set_tests_properties(Smile.RenderPassRegistry PROPERTIES
    LABELS "graphics;architecture;render-pass"
)
