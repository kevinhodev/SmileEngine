add_executable(SmileMathPrimitivesTests
    MathPrimitivesTests.cpp
)

target_compile_features(SmileMathPrimitivesTests PRIVATE cxx_std_20)
target_include_directories(SmileMathPrimitivesTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
set_target_properties(SmileMathPrimitivesTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.MathPrimitives
    COMMAND SmileMathPrimitivesTests
)

set_tests_properties(Smile.MathPrimitives PROPERTIES
    LABELS "math;geometry;matrix"
)

add_executable(SmileRadianceCacheMathTests
    RadianceCacheMathTests.cpp
)

target_compile_features(SmileRadianceCacheMathTests PRIVATE cxx_std_20)
set_target_properties(SmileRadianceCacheMathTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.RadianceCacheMath
    COMMAND SmileRadianceCacheMathTests
)

set_tests_properties(Smile.RadianceCacheMath PROPERTIES
    LABELS "graphics;gi;radiance-cache;math"
)

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

add_executable(SmileSkyAmbientSHTests
    SkyAmbientSHTests.cpp
)

target_compile_features(SmileSkyAmbientSHTests PRIVATE cxx_std_20)
set_target_properties(SmileSkyAmbientSHTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.SkyAmbientSH
    COMMAND SmileSkyAmbientSHTests
)

set_tests_properties(Smile.SkyAmbientSH PROPERTIES
    LABELS "atmosphere;sky;sh;math"
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

add_executable(SmileOpacityMicromapBakeTests
    OpacityMicromapBakeTests.cpp
)

target_compile_features(SmileOpacityMicromapBakeTests PRIVATE cxx_std_20)
target_link_libraries(SmileOpacityMicromapBakeTests PRIVATE SmileEngine)
set_target_properties(SmileOpacityMicromapBakeTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.OpacityMicromapBake.BistroExterior
    COMMAND SmileOpacityMicromapBakeTests
            "${PROJECT_SOURCE_DIR}/Assets/Scenes/Bistro/BistroExterior.sscene"
)

set_tests_properties(Smile.OpacityMicromapBake.BistroExterior PROPERTIES
    LABELS "graphics;raytracing;omm;integration"
    RESOURCE_LOCK CookedSceneMemory
    TIMEOUT 300
)

# Harness de carga da cena cozida, independente de OMM. O SmileOpacityMicromapBakeTests so
# validava index buffer de malha ALPHA-TESTED e pulava renderavel com indice invalido, entao nas
# cenas sem material masked (Sponza, BEEPLE) ele nao validava indice nenhum. Este roda em TODAS.
add_executable(SmileCookedSceneLoadTests
    CookedSceneLoadTests.cpp
)

target_compile_features(SmileCookedSceneLoadTests PRIVATE cxx_std_20)
target_link_libraries(SmileCookedSceneLoadTests PRIVATE SmileEngine)
set_target_properties(SmileCookedSceneLoadTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

# Uma cena por teste: falha aponta o arquivo, e ctest -R filtra por familia. Os assets sao
# gitignored, entao o teste so e registrado quando o .sscene existe na arvore.
foreach(SCENE
    "Bistro/BistroExterior"
    "Bistro/BistroInterior"
    "Bistro/BistroInterior_Wine"
    "EmeraldSquare_v4_1/EmeraldSquare_Day"
    "EmeraldSquare_v4_1/EmeraldSquare_Dusk"
    "SunTemple/SunTemple"
    "main_sponza/NewSponza_Main_Yup_003"
    "main_sponza/NewSponza_Main_Zup_003"
    "BEEPLE Zero-Day/MEASURE_ONE/MEASURE_ONE"
    "BEEPLE Zero-Day/MEASURE_SEVEN/MEASURE_SEVEN"
    "BEEPLE Zero-Day/MEASURE_SEVEN/MEASURE_SEVEN_COLORED_LIGHTS"
)
    set(SCENE_FILE "${PROJECT_SOURCE_DIR}/Assets/Scenes/${SCENE}.sscene")
    if(EXISTS "${SCENE_FILE}")
        get_filename_component(SCENE_NAME "${SCENE}" NAME)
        add_test(
            NAME Smile.CookedSceneLoad.${SCENE_NAME}
            COMMAND SmileCookedSceneLoadTests "${SCENE_FILE}"
        )
        # RESOURCE_LOCK compartilhado com o teste de OMM: cada processo materializa a cena
        # inteira em memoria (o maior .smesh tem ~478 MiB, os 11 somam ~2,6 GB), entao
        # "ctest -j" poderia lancar os 11 ao mesmo tempo e estourar a RAM de maquina menor/CI.
        set_tests_properties(Smile.CookedSceneLoad.${SCENE_NAME} PROPERTIES
            LABELS "scene;cooked-format;integration"
            RESOURCE_LOCK CookedSceneMemory
            TIMEOUT 300
        )
    endif()
endforeach()

# --- Regua de escala: cenas sinteticas -----------------------------------------------------
#
# Geradas DENTRO do diretorio de build, por uma fixture do ctest, e nao em Assets/Scenes/.
# Assets/Scenes/ esta no .gitignore, entao registrar por `if(EXISTS)` fazia o gate depender de
# arquivos locais: num checkout limpo os tres testes sumiam em silencio e a suite continuava
# verde com 20 em vez de 23 — um gate que desaparece sozinho nao e gate.
#
# A RECEITA (contagens e flags) fica versionada aqui, e as mesmas contagens sao passadas ao
# harness como --expect-*: assim o teste prova que o arquivo tem a escala anunciada, em vez de
# so conferir o arquivo consigo mesmo.
set(SMILE_SYNTHETIC_DIR "${CMAKE_CURRENT_BINARY_DIR}/SyntheticScenes")

# instancias;malhas;materiais;masked% — masked_materiais = ceil(materiais * pct / 100)
# Separador "|" e nao ";": num foreach o CMake ACHATA o ";" das strings e cada caso viraria
# seis itens soltos. Convertido para lista logo abaixo.
set(SMILE_SYNTHETIC_CASES
    "10k|10000|8|16|25|4"
    "100k|100000|8|16|25|4"
    "1M|1000000|8|16|25|4"
)

set(SMILE_SYNTHETIC_OUTPUTS "")
foreach(CASE ${SMILE_SYNTHETIC_CASES})
    string(REPLACE "|" ";" CASE_FIELDS "${CASE}")
    list(GET CASE_FIELDS 0 SYN_LABEL)
    list(GET CASE_FIELDS 1 SYN_INSTANCES)
    list(GET CASE_FIELDS 2 SYN_MESHES)
    list(GET CASE_FIELDS 3 SYN_MATERIALS)
    list(GET CASE_FIELDS 4 SYN_MASKED_PCT)
    list(GET CASE_FIELDS 5 SYN_MASKED_COUNT)

    add_test(
        NAME Smile.SyntheticScene.Generate.${SYN_LABEL}
        COMMAND SmileCooker --synthetic "${SMILE_SYNTHETIC_DIR}/Synthetic_${SYN_LABEL}"
                --instances ${SYN_INSTANCES}
                --meshes ${SYN_MESHES}
                --materials ${SYN_MATERIALS}
                --masked-percent ${SYN_MASKED_PCT}
    )
    set_tests_properties(Smile.SyntheticScene.Generate.${SYN_LABEL} PROPERTIES
        LABELS "scene;synthetic;fixture"
        FIXTURES_SETUP SyntheticScene_${SYN_LABEL}
        RESOURCE_LOCK CookedSceneMemory
        TIMEOUT 300
    )

    add_test(
        NAME Smile.CookedSceneLoad.Synthetic_${SYN_LABEL}
        COMMAND SmileCookedSceneLoadTests
                "${SMILE_SYNTHETIC_DIR}/Synthetic_${SYN_LABEL}.sscene"
                --expect-renderables ${SYN_INSTANCES}
                --expect-meshes ${SYN_MESHES}
                --expect-materials ${SYN_MATERIALS}
                --expect-masked ${SYN_MASKED_COUNT}
    )
    set_tests_properties(Smile.CookedSceneLoad.Synthetic_${SYN_LABEL} PROPERTIES
        LABELS "scene;synthetic;cooked-format;integration"
        FIXTURES_REQUIRED SyntheticScene_${SYN_LABEL}
        RESOURCE_LOCK CookedSceneMemory
        TIMEOUT 300
    )
endforeach()

# --- Contratos de FALHA do gerador sintetico ------------------------------------------------
#
# Estes testes existem porque este codigo ja regrediu em varias bordas e o ctest continuava
# verde: a suite so exercitava os tres caminhos VALIDOS. Cada caso abaixo corresponde a um bug
# real que foi corrigido, e o teste existe para ele nao voltar.
#
# WILL_FAIL cobre "sai != 0"; o PASS_REGULAR_EXPRESSION cobriria a mensagem, mas o cooker imprime
# em stdout e o ctest ja trata o exit code como o contrato — o que importa aqui e nao produzir
# arquivo e nao sair com exito.
set(SMILE_SYNTHETIC_BAD_CASES
    # nome|argumentos
    "TessZero|--tess|0"                       # gerava AABB invertida e imprimia OK
    "TessNaN|--tess|abc"                      # strtoul sem end pointer aceitava
    "InstancesZero|--instances|0"
    "InstancesNegative|--instances|-1"         # strtoul(-1) = ULONG_MAX; derrubava o processo
    "TessNegative|--tess|-5"
    "InstancesOverBudget|--instances|999999999"
    "MeshesOverBudget|--meshes|99999"
    "TessOverBudget|--tess|9999"
    "SpacingNegative|--spacing|-1"
    "SpacingNaN|--spacing|nan"
    "MaskedOutOfRange|--masked-percent|150"
    "UnknownFlag|--spcing|99"                  # typo virava posicional e usava o default
    "DerivedCostExplodes|--meshes|4096"        # com --tess default, a tesselacao cresce com M*2
)

foreach(BAD ${SMILE_SYNTHETIC_BAD_CASES})
    string(REPLACE "|" ";" BAD_FIELDS "${BAD}")
    list(GET BAD_FIELDS 0 BAD_NAME)
    list(GET BAD_FIELDS 1 BAD_FLAG)
    list(GET BAD_FIELDS 2 BAD_VALUE)
    add_test(
        NAME Smile.SyntheticScene.Reject.${BAD_NAME}
        COMMAND SmileCooker --synthetic
                "${CMAKE_CURRENT_BINARY_DIR}/SyntheticReject/${BAD_NAME}"
                ${BAD_FLAG} ${BAD_VALUE}
    )
    set_tests_properties(Smile.SyntheticScene.Reject.${BAD_NAME} PROPERTIES
        LABELS "scene;synthetic;negative"
        WILL_FAIL TRUE
        TIMEOUT 60
    )
endforeach()

# Dois posicionais: o modo sintetico consumia so o primeiro e ignorava o resto em silencio.
add_test(
    NAME Smile.SyntheticScene.Reject.ExtraPositional
    COMMAND SmileCooker --synthetic
            "${CMAKE_CURRENT_BINARY_DIR}/SyntheticReject/A"
            "${CMAKE_CURRENT_BINARY_DIR}/SyntheticReject/B"
)
set_tests_properties(Smile.SyntheticScene.Reject.ExtraPositional PROPERTIES
    LABELS "scene;synthetic;negative"
    WILL_FAIL TRUE
    TIMEOUT 60
)

# Rollback da publicacao. Precisa de script porque exige travar um arquivo enquanto o cooker
# roda — o bug era destrutivo (apagava o par valido), entao ele ganha teste proprio.
add_test(
    NAME Smile.SyntheticScene.PublishRollback
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/SyntheticRollbackTest.ps1"
            -Cooker "$<TARGET_FILE:SmileCooker>"
            -LoadTest "$<TARGET_FILE:SmileCookedSceneLoadTests>"
            -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/SyntheticRollback"
)
set_tests_properties(Smile.SyntheticScene.PublishRollback PROPERTIES
    LABELS "scene;synthetic;negative;integration"
    TIMEOUT 120
)
