# SmileCooker — ferramenta offline FBX -> formato proprio (.smesh/.sscene).
# Console app standalone: linka ufbx (single-file) e usa apenas headers da engine
# (CookedFormat.h, Mesh.h) — NAO linka a lib SmileEngine nem D3D12.

set(UFBX_DIR ${CMAKE_SOURCE_DIR}/Engine/ThirdParty/ufbx)

add_executable(SmileCooker
    main.cpp
    ${UFBX_DIR}/ufbx.c
)

target_include_directories(SmileCooker PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Include
    ${UFBX_DIR}
)

# ufbx usa fopen/strncpy etc.; silencia os C4996 do MSVC.
target_compile_definitions(SmileCooker PRIVATE _CRT_SECURE_NO_WARNINGS)

# ufbx.c e C de terceiros: compila sem os /W4 /permissive- do projeto.
set_source_files_properties(${UFBX_DIR}/ufbx.c PROPERTIES COMPILE_FLAGS "/W0")

set_target_properties(SmileCooker PROPERTIES FOLDER "Tools")
