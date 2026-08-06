# Executavel Qt do editor - SmileEditor

# Headers com Q_OBJECT precisam ser listados explicitamente para o AUTOMOC
# por estarem em Include/ separado de Source/ (lição da estrutura SmileEngine)
set(EDITOR_HEADERS
    Include/SmileEditor/MainWindow.h
    Include/SmileEditor/ViewportWidget.h
    Include/SmileEditor/RenderThread.h
    Include/SmileEditor/DarkTheme.h
    Include/SmileEditor/SplashScreen.h
    Include/SmileEditor/SmileLogo.h
    Include/SmileEditor/LucideIcon.h
    Include/SmileEditor/GizmoController.h
    Include/SmileEditor/LogBridge.h
    Include/SmileEditor/QmlHost.h
    Include/SmileEditor/WindowBridge.h
    Include/SmileEditor/NativeWindowFilter.h
    Include/SmileEditor/SmileLogoImageProvider.h
    Include/SmileEditor/MenuBridge.h
    Include/SmileEditor/StatusBridge.h
    Include/SmileEditor/TimeOfDayBridge.h
    Include/SmileEditor/LightsBridge.h
    Include/SmileEditor/SceneOutlinerBridge.h
    Include/SmileEditor/MaterialsBridge.h
    Include/SmileEditor/RenderSettingsBridge.h
)

set(EDITOR_SOURCES
    Source/main.cpp
    Source/MainWindow.cpp
    Source/ViewportWidget.cpp
    Source/RenderThread.cpp
    Source/DarkTheme.cpp
    Source/SplashScreen.cpp
    Source/SmileLogo.cpp
    Source/LucideIcon.cpp
    Source/GizmoController.cpp
    Source/LogBridge.cpp
    Source/QmlHost.cpp
    Source/WindowBridge.cpp
    Source/NativeWindowFilter.cpp
    Source/MenuBridge.cpp
    Source/StatusBridge.cpp
    Source/TimeOfDayBridge.cpp
    Source/LightsBridge.cpp
    Source/SceneOutlinerBridge.cpp
    Source/MaterialsBridge.cpp
    Source/RenderSettingsBridge.cpp
)

# Arquivos de interface aparecem no projeto do Visual Studio para edicao e navegacao.
# Continuam carregados do disco pelo QmlHost (hot-reload) e copiados no pos-build.
file(GLOB_RECURSE EDITOR_QML_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Qml/*.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/Qml/qmldir"
)
file(GLOB_RECURSE EDITOR_QML_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Qml/*.svg"
)
set_source_files_properties(${EDITOR_QML_FILES} ${EDITOR_QML_ASSETS}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# WIN32 = subsistema Windows (sem console preto atras)
qt_add_executable(SmileEditor WIN32
    ${EDITOR_HEADERS}
    ${EDITOR_SOURCES}
    ${EDITOR_QML_FILES}
    ${EDITOR_QML_ASSETS}
)

target_include_directories(SmileEditor
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(SmileEditor
    PRIVATE
        SmileEngine
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Svg
        Qt6::Widgets
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickWidgets
        Qt6::QuickControls2
)

# Barra de titulo frameless (Slate-style): DwmExtendFrameIntoClientArea + hit-test nativo.
if(WIN32)
    target_link_libraries(SmileEditor PRIVATE dwmapi user32)
endif()

# Define o diretório de origem do editor para busca de stylesheets em desenvolvimento
target_compile_definitions(SmileEditor
    PRIVATE
        SMILE_EDITOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SMILE_SHADERS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Shaders"
        SMILE_CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
        SMILE_BUILD_CONFIG="$<CONFIG>"
        SMILE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/Assets"
)

# O Editor depende do build dos shaders para que os .cso existam em runtime
add_dependencies(SmileEditor Shaders)

# Organizacao no Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_HEADERS} ${EDITOR_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Qml" PREFIX "QML"
    FILES ${EDITOR_QML_FILES} ${EDITOR_QML_ASSETS}
)

# Pos-build: copia DLLs do Qt para o lado do executavel via windeployqt
# Necessario para o exe rodar standalone (Qt6Core.dll, Qt6Gui.dll, plataformas, estilos, etc.)
if(WIN32)
    find_program(QT_DEPLOY_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(QT_DEPLOY_EXE)
        add_custom_command(TARGET SmileEditor POST_BUILD
            COMMAND ${QT_DEPLOY_EXE}
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/Qml"
                    "$<TARGET_FILE:SmileEditor>"
            COMMENT "Executando windeployqt para o SmileEditor"
        )
    else()
        message(WARNING "windeployqt nao encontrado - sera necessario copiar as DLLs do Qt manualmente")
    endif()

    # Copia a pasta de Styles para a pasta de saída do executável
    add_custom_command(TARGET SmileEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/Styles"
                "$<TARGET_FILE_DIR:SmileEditor>/Editor/Styles"
        COMMENT "Copiando stylesheets QSS para o diretorio de build"
    )

    # Copia a pasta de Qml (deploy fallback; em dev carregamos do source dir p/ hot-reload)
    add_custom_command(TARGET SmileEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/Qml"
                "$<TARGET_FILE_DIR:SmileEditor>/Editor/Qml"
        COMMENT "Copiando paineis QML para o diretorio de build"
    )

    # Arte da splash screen. Em dev o SplashScreen carrega direto do source dir (trocar o PNG
    # nao precisa de rebuild); esta copia e o caminho de deploy, ao lado do exe.
    add_custom_command(TARGET SmileEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/Resources"
                "$<TARGET_FILE_DIR:SmileEditor>/Editor/Resources"
        COMMENT "Copiando recursos do editor (splash) para o diretorio de build"
    )

    # Fontes empacotadas (Inter, SIL OFL) — registradas no boot por RegisterBundledFonts().
    # Vao em Fonts/ ao lado do exe, que e onde o main.cpp procura.
    add_custom_command(TARGET SmileEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/Fonts"
                "$<TARGET_FILE_DIR:SmileEditor>/Fonts"
        COMMENT "Copiando fontes empacotadas para o diretorio de build"
    )

    # Copia a NRD.dll ao lado do exe (o NRD usa o caminho D3D12 direto via import lib + DLL).
    if(SMILE_NRD_DLL AND EXISTS "${SMILE_NRD_DLL}")
        add_custom_command(TARGET SmileEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SMILE_NRD_DLL}"
                    "$<TARGET_FILE_DIR:SmileEditor>"
            COMMENT "Copiando NRD.dll para o diretorio de build"
        )
    endif()

    # Copia a amd_fidelityfx_dx12.dll ao lado do exe (o FSR/ffx-api usa import lib + DLL, igual ao NRD).
    if(SMILE_FSR_DLL AND EXISTS "${SMILE_FSR_DLL}")
        add_custom_command(TARGET SmileEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SMILE_FSR_DLL}"
                    "$<TARGET_FILE_DIR:SmileEditor>"
            COMMENT "Copiando amd_fidelityfx_dx12.dll para o diretorio de build"
        )
    endif()

    # Copia os DLLs (production) do Streamline/DLSS ao lado do exe (manual hooking: import lib + DLLs).
    foreach(_sl_dll ${SMILE_SL_DLLS})
        if(EXISTS "${_sl_dll}")
            add_custom_command(TARGET SmileEditor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_sl_dll}"
                        "$<TARGET_FILE_DIR:SmileEditor>"
                COMMENT "Copiando ${_sl_dll} para o diretorio de build"
            )
        endif()
    endforeach()
endif()

