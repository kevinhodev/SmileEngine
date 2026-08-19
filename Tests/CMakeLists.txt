add_executable(SmileOceanMathBaselineTests
    OceanMathBaselineTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/OceanSpectrum.cpp
)

target_compile_features(SmileOceanMathBaselineTests PRIVATE cxx_std_20)
target_include_directories(SmileOceanMathBaselineTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
set_target_properties(SmileOceanMathBaselineTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
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
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/GpuMesh.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/GpuResources.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/UploadQueue.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/VramTracker.cpp
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
)

add_test(
    NAME Smile.SceneIdentity
    COMMAND SmileSceneIdentityTests
)

set_tests_properties(Smile.SceneIdentity PROPERTIES
    LABELS "scene;identity;editor"
)

# Importador glTF/GLB do Cooker. Nao usa GPU: cozinhar e transformar bytes em bytes, e e ai que
# os erros deste caminho aparecem (Z/winding, UV, payload de RT, accessor fora do bufferView).
# Compila o GltfImport.cpp direto — o Cooker e um executavel, nao ha lib para linkar.
add_executable(SmileGltfImportTests
    GltfImportTests.cpp
    ${PROJECT_SOURCE_DIR}/Tools/Cooker/GltfImport.cpp
)

target_compile_features(SmileGltfImportTests PRIVATE cxx_std_20)
target_include_directories(SmileGltfImportTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
    ${PROJECT_SOURCE_DIR}/Tools/Cooker
)
# Mesmo motivo do alvo SmileCooker: strncpy/fopen do CookedWriter.h disparam C4996 no MSVC.
target_compile_definitions(SmileGltfImportTests PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(SmileGltfImportTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

add_test(
    NAME Smile.GltfImport
    COMMAND SmileGltfImportTests
)

set_tests_properties(Smile.GltfImport PROPERTIES
    LABELS "cooker;gltf;import;meshgen"
)
