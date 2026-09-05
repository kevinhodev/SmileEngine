option(SMILE_ENABLE_RENDERER_IDENTITY_TESTS "Run Phase 1 integration on a DXR-capable GPU" OFF)
if(SMILE_ENABLE_RENDERER_IDENTITY_TESTS)
    add_executable(SmileRendererIdentityTests RendererIdentityTests.cpp)
    target_link_libraries(SmileRendererIdentityTests PRIVATE SmileEngine)
    target_compile_features(SmileRendererIdentityTests PRIVATE cxx_std_20)
    set_target_properties(SmileRendererIdentityTests PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF FOLDER "Tests")
    # Same output directory and runtime dependencies (Agility/NRD/Streamline) as the editor.
    add_dependencies(SmileRendererIdentityTests SmileEditor Shaders)
    add_test(NAME Smile.RendererIdentity
        COMMAND SmileRendererIdentityTests "${CMAKE_CURRENT_BINARY_DIR}/SyntheticScenes/Synthetic_10k.sscene")
    set_tests_properties(Smile.RendererIdentity PROPERTIES
        LABELS "gpu-driven;identity;hardware" TIMEOUT 240
        FIXTURES_REQUIRED SyntheticScene_10k RESOURCE_LOCK CookedSceneMemory
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:SmileEditor>")
endif()

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
    ${PROJECT_SOURCE_DIR}/Engine/Source/Scene/GpuSceneHandle.cpp
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

# Linha canonica de material da Fase 2. CPU pura: o GpuMaterialRecord.cpp nao aloca recurso e o
# que ele le do FMaterial e tudo inline no header, entao nao ha Material.cpp para linkar.
add_executable(SmileGpuMaterialRecordTests
    GpuMaterialRecordTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Scene/GpuMaterialRecord.cpp
)

target_compile_features(SmileGpuMaterialRecordTests PRIVATE cxx_std_20)
target_include_directories(SmileGpuMaterialRecordTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
set_target_properties(SmileGpuMaterialRecordTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.GpuMaterialRecord
    COMMAND SmileGpuMaterialRecordTests
)

set_tests_properties(Smile.GpuMaterialRecord PROPERTIES
    LABELS "scene;materials;gpu-driven;abi"
)

# ABI C++ <-> HLSL das tabelas canonicas, executada em WARP, mais a parte CPU pura da tabela
# (DiffMaterialRows). Depende da sonda compilada pelo target Shaders.
add_executable(SmileGpuSceneTablesWarpTests
    GpuSceneTablesWarpTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Scene/GpuMaterialRecord.cpp
)
target_compile_features(SmileGpuSceneTablesWarpTests PRIVATE cxx_std_20)
target_include_directories(SmileGpuSceneTablesWarpTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
target_compile_definitions(SmileGpuSceneTablesWarpTests PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
    SMILE_SCENE_TABLES_PROBE_CSO="${SMILE_SHADER_OUTPUT_DIR}/GpuSceneTablesProbe.cs_6_0.cso"
)
target_link_libraries(SmileGpuSceneTablesWarpTests PRIVATE d3d12 dxgi)
add_dependencies(SmileGpuSceneTablesWarpTests Shaders)
set_target_properties(SmileGpuSceneTablesWarpTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.GpuSceneTablesWarp
    COMMAND SmileGpuSceneTablesWarpTests
)

set_tests_properties(Smile.GpuSceneTablesWarp PROPERTIES
    LABELS "scene;materials;gpu-driven;abi;warp"
    TIMEOUT 180
)

# Ciclo de vida dos descritores de SRV (Fase 3). Linka a engine porque o FTextureSRVHeap cria
# heaps de descritor de verdade; o device e WARP, nada aqui depende de GPU real.
add_executable(SmileDescriptorLifetimeTests
    DescriptorLifetimeTests.cpp
)

target_compile_features(SmileDescriptorLifetimeTests PRIVATE cxx_std_20)
target_link_libraries(SmileDescriptorLifetimeTests PRIVATE SmileEngine d3d12 dxgi)
set_target_properties(SmileDescriptorLifetimeTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.DescriptorLifetime
    COMMAND SmileDescriptorLifetimeTests
)

set_tests_properties(Smile.DescriptorLifetime PROPERTIES
    LABELS "graphics;materials;gpu-driven;lifetime;warp"
    TIMEOUT 120
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
# Cada caso roda por SyntheticRejectTest.ps1, que confere o contrato INTEIRO: exit exatamente 2,
# diagnostico impresso e nenhuma saida escrita ou removida. O WILL_FAIL do ctest provaria apenas
# "retorno != 0" — e com sentinelas no destino um caso passava com os arquivos intactos no lugar,
# ou seja, o gate nao distinguia "recusou" de "recusou e sujou".
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
    # Wrapper em vez de WILL_FAIL: aquele prova so "retorno != 0". Com sentinelas no destino,
    # um caso passava E os arquivos permaneciam — falso positivo contra o contrato declarado
    # ("exit 2 e zero arquivos"). O script confere exit exato, diagnostico e saidas intocadas.
    add_test(
        NAME Smile.SyntheticScene.Reject.${BAD_NAME}
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/SyntheticRejectTest.ps1"
                -Cooker "$<TARGET_FILE:SmileCooker>"
                -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/SyntheticReject"
                -CaseName ${BAD_NAME}
                -BadArgsCsv "${BAD_FLAG},${BAD_VALUE}"
    )
    set_tests_properties(Smile.SyntheticScene.Reject.${BAD_NAME} PROPERTIES
        LABELS "scene;synthetic;negative"
        TIMEOUT 60
    )
endforeach()

# Dois posicionais: o modo sintetico consumia so o primeiro e ignorava o resto em silencio.
add_test(
    NAME Smile.SyntheticScene.Reject.ExtraPositional
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/SyntheticRejectTest.ps1"
            -Cooker "$<TARGET_FILE:SmileCooker>"
            -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/SyntheticReject"
            -CaseName ExtraPositional
            -BadArgsCsv "${CMAKE_CURRENT_BINARY_DIR}/SyntheticReject/Extra"
)
set_tests_properties(Smile.SyntheticScene.Reject.ExtraPositional PROPERTIES
    LABELS "scene;synthetic;negative"
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

# Ramos do publicador de par do cooker. Teste C++ porque o ramo "primeiro publicado, segundo
# falhou" NAO e alcancavel de fora do processo: travar um destino faz a falha cair no backup, ja
# que os dois backups acontecem antes de qualquer publicacao. Aqui o filesystem e injetado.
add_executable(SmileCookerPublishBundleTests
    CookerPublishBundleTests.cpp
)
target_compile_features(SmileCookerPublishBundleTests PRIVATE cxx_std_20)
set_target_properties(SmileCookerPublishBundleTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

add_test(
    NAME Smile.Cooker.PublishBundle
    COMMAND SmileCookerPublishBundleTests
)
set_tests_properties(Smile.Cooker.PublishBundle PROPERTIES
    LABELS "cooker;negative"
    TIMEOUT 60
)

# Valida o sidecar de bookmarks pelo PARSER REAL do editor. Sem isto, o gate sintetico so
# passava o .sscene ao loader: remover a geracao do .cameras.json, emitir JSON invalido ou
# escrever indices errados continuava verde.
add_executable(SmileBookmarkSidecarTests
    BookmarkSidecarTests.cpp
)
target_compile_features(SmileBookmarkSidecarTests PRIVATE cxx_std_20)
target_include_directories(SmileBookmarkSidecarTests PRIVATE
    "${PROJECT_SOURCE_DIR}/Editor/Include"
)
# So Qt Core: o BookmarkSchema nao depende de QObject nem do Renderer, que e exatamente o
# motivo de ele ter sido extraido do bridge.
target_link_libraries(SmileBookmarkSidecarTests PRIVATE Qt6::Core)
set_target_properties(SmileBookmarkSidecarTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

foreach(CASE ${SMILE_SYNTHETIC_CASES})
    string(REPLACE "|" ";" CASE_FIELDS "${CASE}")
    list(GET CASE_FIELDS 0 SYN_LABEL)
    add_test(
        NAME Smile.BookmarkSidecar.Synthetic_${SYN_LABEL}
        COMMAND SmileBookmarkSidecarTests
                "${SMILE_SYNTHETIC_DIR}/Synthetic_${SYN_LABEL}.cameras.json"
                --expect-slots 4
    )
    set_tests_properties(Smile.BookmarkSidecar.Synthetic_${SYN_LABEL} PROPERTIES
        LABELS "scene;synthetic;bookmarks"
        FIXTURES_REQUIRED SyntheticScene_${SYN_LABEL}
        TIMEOUT 60
    )
endforeach()

# Os sidecars VERSIONADOS das cenas reais tambem passam pelo parser: eles entram no repositorio
# e vao ser editados a mao, entao um JSON quebrado ali precisa derrubar o gate e nao aparecer
# so na hora da captura.
#
# Formato: "<caminho sem extensao>|<slots exigidos, ou 0 para 'pelo menos um'>".
#
# As duas cenas de GATE (Bistro e Emerald) exigem os 4: as poses ja foram autoradas, e com o
# numero fixado o gate passa a defender o baseline em vez de so conferir sintaxe — apagar ou
# duplicar um slot vira falha aqui, e nao uma medicao silenciosamente diferente semanas depois.
# --expect-slots tambem torna ESTRITA a checagem de poses distintas, que e exatamente a regressao
# que a Emerald tinha (slots 0 e 1 byte-a-byte iguais).
#
# As cenas OPCIONAIS ficam em 0: nelas as poses sao trabalho em aberto, e travar um numero faria
# a suite ficar vermelha por autoria pendente — suite permanentemente vermelha para de informar.
foreach(REAL_SIDECAR
    "Bistro/BistroExterior|4"
    "EmeraldSquare_v4_1/EmeraldSquare_Day|4"
    "BEEPLE Zero-Day/MEASURE_SEVEN/MEASURE_SEVEN_COLORED_LIGHTS|0"
)
    # Separador "|" e nao ";": o CMake achata ";" dentro de listas de foreach.
    string(REPLACE "|" ";" SIDECAR_FIELDS "${REAL_SIDECAR}")
    list(GET SIDECAR_FIELDS 0 SIDECAR_PATH)
    list(GET SIDECAR_FIELDS 1 SIDECAR_SLOTS)

    # SEM if(EXISTS): estes sidecars sao VERSIONADOS, entao a ausencia e regressao e nao
    # ambiente. Registrar condicionalmente faria um checkout sem eles rodar menos testes e
    # continuar verde — o mesmo gate-que-some-sozinho que as cenas sinteticas ja tiveram. O
    # executavel devolve exit 3 quando o arquivo nao esta la, que e o diagnostico certo.
    set(SIDECAR_FILE "${PROJECT_SOURCE_DIR}/Assets/Scenes/${SIDECAR_PATH}.cameras.json")
    get_filename_component(SIDECAR_NAME "${SIDECAR_PATH}" NAME)

    set(SIDECAR_ARGS "")
    if(NOT SIDECAR_SLOTS EQUAL 0)
        set(SIDECAR_ARGS --expect-slots ${SIDECAR_SLOTS})
    endif()

    add_test(
        NAME Smile.BookmarkSidecar.${SIDECAR_NAME}
        COMMAND SmileBookmarkSidecarTests "${SIDECAR_FILE}" ${SIDECAR_ARGS}
    )
    set_tests_properties(Smile.BookmarkSidecar.${SIDECAR_NAME} PROPERTIES
        LABELS "scene;bookmarks"
        TIMEOUT 60
    )
endforeach()

# Testes do publicador de baselines (Tools/SmileMCP/scripts/promote-baseline.mjs).
#
# Ele escreve em Docs/Baselines, que e versionado, e nunca teve teste: todos os defeitos dele
# foram achados por revisao humana lendo o codigo — escape de diretorio pelo --as, sobrescrita
# silenciosa da baseline anterior, publicacao nao transacional, manifesto apontando para PNG
# inexistente. Cada um desses casos virou teste, num diretorio temporario (SMILE_BASELINE_DIR).
#
# CONDICIONAL ao node existir, ao contrario dos sidecars versionados: o node nao e dependencia de
# build da engine, e exigi-lo aqui quebraria o gate de quem so compila C++. A ausencia aparece no
# configure, entao nao e um gate que some sem avisar.
find_program(SMILE_NODE_EXECUTABLE NAMES node node.exe)
if(SMILE_NODE_EXECUTABLE)
    add_test(
        NAME Smile.PromoteBaseline
        COMMAND "${SMILE_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Tests/PromoteBaselineTests.mjs"
    )
    set_tests_properties(Smile.PromoteBaseline PROPERTIES
        LABELS "tools;baselines"
        TIMEOUT 180
    )
else()
    message(STATUS "node nao encontrado: Smile.PromoteBaseline nao sera registrado")
endif()

# Benchmark do custo de remocao estrutural na FScene, por posicao na lista.
#
# Mede, nao afirma: a Fase 1 mantem o `erase` estavel de proposito, e o preco declarado dessa
# escolha e O(N) por edicao de editor. "Irrelevante" e uma afirmacao sobre magnitude, e magnitude
# se mede. Sem assercao de tempo — cravar um limite aqui daria um teste que falha por a maquina
# estar ocupada, e isso para de informar.
#
# Reaproveita a mesma lista de fontes do SmileSceneIdentityTests: o mesmo recorte de CPU pura,
# sem device.
add_executable(SmileSceneRemovalBenchmark
    SceneRemovalBenchmark.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Scene/GpuSceneHandle.cpp
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
target_compile_features(SmileSceneRemovalBenchmark PRIVATE cxx_std_20)
target_include_directories(SmileSceneRemovalBenchmark PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
    ${PROJECT_SOURCE_DIR}/Engine/ThirdParty
)
target_link_libraries(SmileSceneRemovalBenchmark PRIVATE d3d12 dxgi)
set_target_properties(SmileSceneRemovalBenchmark PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)

# No ctest com contagem PEQUENA, so para o caminho nao apodrecer. A medicao de 1M e manual, para
# nao pendurar dois segundos e ~500 MB em cada rodada da suite.
add_test(
    NAME Smile.SceneRemovalBenchmark.Smoke
    COMMAND SmileSceneRemovalBenchmark --count 20000
)
set_tests_properties(Smile.SceneRemovalBenchmark.Smoke PROPERTIES
    LABELS "scene;benchmark"
    TIMEOUT 120
)

# Casos NEGATIVOS do parser do benchmark.
#
# Um script, e nao PASS_REGULAR_EXPRESSION do ctest: com ela o ctest IGNORA o codigo de retorno,
# entao o gate provaria a mensagem e aceitaria a violacao de acesso — que e exatamente o defeito
# que ele existe para impedir (o `--count 0` derrubava o processo com 0xC0000005). Mesmo argumento
# do gate do cooker acima, e mesmo padrao: o script confere exit EXATAMENTE 2, o diagnostico, e
# que o caminho valido continua passando.
add_test(
    NAME Smile.SceneRemovalBenchmark.Reject
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/SceneRemovalRejectTest.ps1"
            -Benchmark "$<TARGET_FILE:SmileSceneRemovalBenchmark>"
)
set_tests_properties(Smile.SceneRemovalBenchmark.Reject PROPERTIES
    LABELS "scene;benchmark;negative"
    TIMEOUT 120
)

# Ciclo de vida do FGpuSceneHandle (Fase 1 do GPU-DRIVEN-PLAN).
#
# CPU puro: o allocator recebe NUMEROS do renderer (FGpuFenceSet) e nao conhece D3D12, e e
# exatamente isso que permite cobrir generation, handle obsoleto, wrap, reload e reuso por fence
# sem device e sem GPU no CI.
add_executable(SmileGpuSceneHandleTests
    GpuSceneHandleTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Scene/GpuSceneHandle.cpp
)
target_compile_features(SmileGpuSceneHandleTests PRIVATE cxx_std_20)
target_include_directories(SmileGpuSceneHandleTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
set_target_properties(SmileGpuSceneHandleTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)
add_test(
    NAME Smile.GpuSceneHandle
    COMMAND SmileGpuSceneHandleTests
)
set_tests_properties(Smile.GpuSceneHandle PROPERTIES
    LABELS "scene;identity;gpu-driven"
    TIMEOUT 60
)

# Validacao de borda do snapshot de perfil, no bridge TypeScript.
#
# O protocolo 4 subiu para EXIGIR cpu.gpuSlots, e exigencia sem validacao existe so no numero da
# versao: um payload sem o bloco atravessava o cast e quebrava adiante no summarizeProfile com
# TypeError, longe da causa. O teste exercita o bridge por um FAKE de named pipe -- o que esta sob
# teste e a validacao, nao o transporte.
#
# O TESTE compila o bridge sozinho, para um diretorio temporario proprio. Antes ele importava de
# Tools/SmileMCP/dist, que e ignorado pelo git e nao e produzido por fixture nenhuma: num checkout
# limpo o teste morria com ERR_MODULE_NOT_FOUND, e com um dist antigo em disco ficava VERDE
# validando codigo que nao era o do fonte. CONDICIONAL ao node, como o Smile.PromoteBaseline, e
# pela mesma razao: node nao e dependencia de build da engine.
# O teste compila o bridge com o tsc de node_modules, entao o node sozinho nao basta: sem
# "npm ci" em Tools/SmileMCP o tsc nao existe e o teste falharia com MODULE_NOT_FOUND.
# Verificado aqui de proposito, em vez de descoberto na execucao, e a mensagem no configure diz o
# que fazer.
#
# Registrar condicionalmente e o OPOSTO do que se faz com sidecar versionado (ali a ausencia e
# regressao e o gate tem de cair). Aqui a ausencia e de dependencia de terceiro, nao rastreada,
# e um teste vermelho por ambiente para de informar. O proprio teste tambem confere, para quem o
# invocar direto.
set(SMILE_MCP_TSC "${PROJECT_SOURCE_DIR}/Tools/SmileMCP/node_modules/typescript/bin/tsc")
if(SMILE_NODE_EXECUTABLE AND NOT EXISTS "${SMILE_MCP_TSC}")
    message(STATUS
        "Tools/SmileMCP/node_modules ausente: Smile.EditorBridgeValidation nao sera registrado "
        "(rode 'npm ci' em Tools/SmileMCP)")
endif()

if(SMILE_NODE_EXECUTABLE AND EXISTS "${SMILE_MCP_TSC}")
    add_test(
        NAME Smile.EditorBridgeValidation
        COMMAND "${SMILE_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Tests/EditorBridgeValidationTests.mjs"
    )
    set_tests_properties(Smile.EditorBridgeValidation PROPERTIES
        LABELS "tools;mcp;negative"
        TIMEOUT 120
    )
endif()

# Historico por slot de residencia. CPU puro: sem device, sem d3d12.
#
# Existe porque o teste da migracao verificava a estabilidade dos HANDLES e nao o historico --
# reverter para indice posicional, ou remover a comparacao de generation, nao mudaria nada nele.
# Aqui os VALORES guardados sao conferidos, com matrizes distintas por objeto e por frame.
add_executable(SmileSlotKeyedHistoryTests
    SlotKeyedHistoryTests.cpp
)
target_compile_features(SmileSlotKeyedHistoryTests PRIVATE cxx_std_20)
target_include_directories(SmileSlotKeyedHistoryTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
# Caminho do decoder de producao. O teste le o FONTE do shader porque executar HLSL exigiria GPU,
# e o criterio que importa e que reintroduzir o "+0.5f" la deixe este gate vermelho.
target_compile_definitions(SmileSlotKeyedHistoryTests PRIVATE
    SMILE_TEMPORAL_COMMON_PATH="${PROJECT_SOURCE_DIR}/Shaders/Temporal/TemporalMotionCommon.hlsli"
)
set_target_properties(SmileSlotKeyedHistoryTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)
add_test(
    NAME Smile.SlotKeyedHistory
    COMMAND SmileSlotKeyedHistoryTests
)
set_tests_properties(Smile.SlotKeyedHistory PROPERTIES
    LABELS "scene;identity;gpu-driven"
    TIMEOUT 60
)

# Tabela de identidades por frame + remap de slot. So precisa do allocator: o preenchimento e
# header-only e templado no acessor, de proposito, para que teste e producao rodem o mesmo codigo.
add_executable(SmileTemporalIdentityTableTests
    TemporalIdentityTableTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Scene/GpuSceneHandle.cpp
)
target_compile_features(SmileTemporalIdentityTableTests PRIVATE cxx_std_20)
target_include_directories(SmileTemporalIdentityTableTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
target_link_libraries(SmileTemporalIdentityTableTests PRIVATE d3d12 dxgi)
# Os dois fontes que o gate le. Executar HLSL exigiria GPU; o criterio que importa e que reverter
# o leitor para indexar transform com id antigo deixe o gate vermelho.
target_compile_definitions(SmileTemporalIdentityTableTests PRIVATE
    SMILE_DUALMOTION_PATH="${PROJECT_SOURCE_DIR}/Shaders/Temporal/DualMotion.cs.hlsl"
    SMILE_REFLECTIONMOTION_PATH="${PROJECT_SOURCE_DIR}/Shaders/Reflections/ReflectionMotion.hlsli"
    SMILE_DISPATIAL_PATH="${PROJECT_SOURCE_DIR}/Shaders/Lighting/ReSTIRDISpatial.cs.hlsl"
    SMILE_RENDERERSCENE_PATH="${PROJECT_SOURCE_DIR}/Engine/Source/Graphics/Renderer/RendererScene.cpp"
)
set_target_properties(SmileTemporalIdentityTableTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)
add_test(
    NAME Smile.TemporalIdentityTable
    COMMAND SmileTemporalIdentityTableTests
)
set_tests_properties(Smile.TemporalIdentityTable PROPERTIES
    LABELS "scene;identity;gpu-driven"
    TIMEOUT 60
)

# Execucao real do resolvedor de identidade temporal, em WARP (software), substituindo o gate de
# strings que nao pegava inversao de operador. Depende da sonda compilada pelo target Shaders.
add_executable(SmileTemporalResolveWarpTests
    TemporalResolveWarpTests.cpp
    ${PROJECT_SOURCE_DIR}/Engine/Source/Scene/GpuSceneHandle.cpp
)
target_compile_features(SmileTemporalResolveWarpTests PRIVATE cxx_std_20)
target_include_directories(SmileTemporalResolveWarpTests PRIVATE
    ${PROJECT_SOURCE_DIR}/Engine/Include
)
target_compile_definitions(SmileTemporalResolveWarpTests PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
    SMILE_RESOLVE_PROBE_CSO="${SMILE_SHADER_OUTPUT_DIR}/TemporalResolveProbe.cs_6_0.cso"
)
target_link_libraries(SmileTemporalResolveWarpTests PRIVATE d3d12 dxgi)
add_dependencies(SmileTemporalResolveWarpTests Shaders)
set_target_properties(SmileTemporalResolveWarpTests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    FOLDER "Tests"
)
add_test(
    NAME Smile.TemporalResolveWarp
    COMMAND SmileTemporalResolveWarpTests
)
set_tests_properties(Smile.TemporalResolveWarp PROPERTIES
    LABELS "scene;identity;gpu-driven;warp"
    TIMEOUT 180
)
