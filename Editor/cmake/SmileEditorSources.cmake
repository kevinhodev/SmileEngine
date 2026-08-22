# Manifesto de fontes e organização do SmileEditor no Visual Studio.
#
# Headers e implementações espelham os mesmos domínios no disco. No Visual Studio, o
# .h e o .cpp de cada componente aparecem juntos, sem filtros Include/Source.

set(SMILE_EDITOR_FILES "")
set(SMILE_EDITOR_ASSET_FILES "")

function(smile_editor_group group)
    set(group_files "")

    foreach(file IN LISTS ARGN)
        get_filename_component(absolute_file "${file}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

        if(NOT EXISTS "${absolute_file}")
            message(FATAL_ERROR
                "SmileEditor source '${file}' in group '${group}' does not exist")
        endif()

        list(APPEND group_files "${absolute_file}")
    endforeach()

    source_group("${group}" FILES ${group_files})
    set(SMILE_EDITOR_FILES
        ${SMILE_EDITOR_FILES}
        ${group_files}
        PARENT_SCOPE
    )
endfunction()

function(smile_editor_domain domain)
    set(domain_files "")

    foreach(component IN LISTS ARGN)
        set(header "Include/SmileEditor/${domain}/${component}.h")
        set(source "Source/${domain}/${component}.cpp")
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
                "Editor component '${component}' does not exist in domain '${domain}'")
        endif()
    endforeach()

    smile_editor_group("${domain}" ${domain_files})
    set(SMILE_EDITOR_FILES ${SMILE_EDITOR_FILES} PARENT_SCOPE)
endfunction()

function(smile_editor_assets group)
    smile_editor_group("${group}" ${ARGN})
    set(SMILE_EDITOR_FILES ${SMILE_EDITOR_FILES} PARENT_SCOPE)
    set(SMILE_EDITOR_ASSET_FILES
        ${SMILE_EDITOR_ASSET_FILES}
        ${ARGN}
        PARENT_SCOPE
    )
endfunction()

smile_editor_domain(Application
    DarkTheme
    LucideIcon
    MainWindow
    NativeWindowFilter
    SmileLogo
    SmileLogoImageProvider
    SplashScreen
)
smile_editor_group("Application" Source/Application/main.cpp)

smile_editor_domain(UI
    LogBridge
    MenuBridge
    QmlHost
    StatusBridge
    WindowBridge
)

smile_editor_domain(Scene
    CameraBookmarksBridge
    CloudsBridge
    JsonSidecar
    LightsBridge
    MaterialsBridge
    OceanBridge
    SceneDocument
    SceneOutlinerBridge
    TimeOfDayBridge
    WeatherBridge
)

smile_editor_domain(Rendering
    CaptureBridge
    RenderSettingsBridge
    RenderSettingsController
)

smile_editor_domain(Integration
    McpBridge
)

smile_editor_domain(Viewport
    GizmoController
    RendererJobQueue
    RenderThread
    ViewportWidget
)

smile_editor_domain(Profiling
    StatsBridge
)

smile_editor_domain(Debugging
    DebugTargetsBridge
)

smile_editor_assets("UI\\QML\\Shell"
    Qml/ConsolePanel.qml
    Qml/EditorMenuBar.qml
    Qml/MainBar.qml
    Qml/StatusBar.qml
)

smile_editor_assets("UI\\QML\\Scene"
    Qml/MaterialsWindow.qml
    Qml/SceneOutlinerPanel.qml
    Qml/TimeOfDayWindow.qml
)

smile_editor_assets("UI\\QML\\Scene\\Inspectors"
    Qml/Scene/CloudsInspector.qml
    Qml/Scene/OceanInspector.qml
    Qml/Scene/WeatherInspector.qml
)

smile_editor_assets("UI\\QML\\Rendering"
    Qml/SettingsWindow.qml
    Qml/ViewportToolbar.qml
)

smile_editor_assets("UI\\QML\\Debugging"
    Qml/DebugTargetsWindow.qml
)

smile_editor_assets("UI\\QML\\Profiling"
    Qml/StatsWindow.qml
)

smile_editor_assets("UI\\QML\\Shared"
    Qml/LucideIcon.qml
    Qml/ThinScrollBar.qml
    Qml/components/Card.qml
    Qml/components/BipolarSliderRow.qml
    Qml/components/Chip.qml
    Qml/components/ColorRow.qml
    Qml/components/InspectorSectionHeader.qml
    Qml/components/PairedScrubRow.qml
    Qml/components/PrimarySliderRow.qml
    Qml/components/ScrubField.qml
    Qml/components/ScrubRow.qml
    Qml/components/SliderRow.qml
    Qml/components/StepperRow.qml
    Qml/components/Theme.qml
    Qml/components/Toggle.qml
    Qml/components/ToggleRow.qml
    Qml/components/ToolbarButton.qml
    Qml/components/ToolbarIcon.qml
    Qml/components/ToolTip.qml
    Qml/components/WindowButton.qml
    Qml/components/qmldir
)

file(GLOB editor_qml_icons CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Qml/icons/*.svg"
)
smile_editor_assets("UI\\QML\\Icons" ${editor_qml_icons})

file(GLOB editor_font_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Fonts/*"
)
file(GLOB editor_resource_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/*"
)
file(GLOB editor_style_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Styles/*"
)
smile_editor_assets("Resources\\Fonts" ${editor_font_files})
smile_editor_assets("Resources" ${editor_resource_files})
smile_editor_assets("UI\\Styles" ${editor_style_files})

set_source_files_properties(${SMILE_EDITOR_ASSET_FILES}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# Arquivos C++ e QML novos exigem domínio explícito. Assets estáticos herdam o grupo
# do diretório físico e também participam das verificações de duplicata e caminho obsoleto.
file(GLOB_RECURSE discovered_editor_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/SmileEditor/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Qml/*.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/Qml/qmldir"
    "${CMAKE_CURRENT_SOURCE_DIR}/Qml/*.svg"
    "${CMAKE_CURRENT_SOURCE_DIR}/Fonts/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Styles/*"
)

set(unique_editor_files ${SMILE_EDITOR_FILES})
list(REMOVE_DUPLICATES unique_editor_files)
list(LENGTH SMILE_EDITOR_FILES editor_file_count)
list(LENGTH unique_editor_files unique_editor_file_count)

if(NOT editor_file_count EQUAL unique_editor_file_count)
    message(FATAL_ERROR "SmileEditor source manifest contains duplicate files")
endif()

set(unclassified_editor_files ${discovered_editor_files})
list(REMOVE_ITEM unclassified_editor_files ${unique_editor_files})

set(stale_editor_files ${unique_editor_files})
list(REMOVE_ITEM stale_editor_files ${discovered_editor_files})

if(unclassified_editor_files)
    string(REPLACE ";" "\n  " unclassified_message "${unclassified_editor_files}")
    message(FATAL_ERROR
        "SmileEditor files without a source group:\n  ${unclassified_message}")
endif()

if(stale_editor_files)
    string(REPLACE ";" "\n  " stale_message "${stale_editor_files}")
    message(FATAL_ERROR
        "SmileEditor source manifest contains missing files:\n  ${stale_message}")
endif()
