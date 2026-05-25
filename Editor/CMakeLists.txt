# Executavel Qt do editor - SmileEditor

# Headers com Q_OBJECT precisam ser listados explicitamente para o AUTOMOC
# por estarem em Include/ separado de Source/ (lição da estrutura SmileEngine)
set(EDITOR_HEADERS
    Include/SmileEditor/MainWindow.h
    Include/SmileEditor/ViewportWidget.h
    Include/SmileEditor/DarkTheme.h
    Include/SmileEditor/AboutDialog.h
    Include/SmileEditor/TextureSlotWidget.h
    Include/SmileEditor/MaterialEditorPanel.h
)

set(EDITOR_SOURCES
    Source/main.cpp
    Source/MainWindow.cpp
    Source/ViewportWidget.cpp
    Source/DarkTheme.cpp
    Source/AboutDialog.cpp
    Source/TextureSlotWidget.cpp
    Source/MaterialEditorPanel.cpp
)

# WIN32 = subsistema Windows (sem console preto atras)
qt_add_executable(SmileEditor WIN32
    ${EDITOR_HEADERS}
    ${EDITOR_SOURCES}
)

target_include_directories(SmileEditor
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(SmileEditor
    PRIVATE
        SmileEngine
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

# O Editor depende do build dos shaders para que os .cso existam em runtime
add_dependencies(SmileEditor Shaders)

# Organizacao no Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EDITOR_HEADERS} ${EDITOR_SOURCES})

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
                    --no-quick-import
                    "$<TARGET_FILE:SmileEditor>"
            COMMENT "Executando windeployqt para o SmileEditor"
        )
    else()
        message(WARNING "windeployqt nao encontrado - sera necessario copiar as DLLs do Qt manualmente")
    endif()
endif()
