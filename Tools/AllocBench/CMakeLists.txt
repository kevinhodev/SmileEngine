# SmileAllocBench — mede quanto do custo de criar recurso e o heap implicito, e quanto disso o
# D3D12MA devolve. Console app standalone: cria o proprio device D3D12 e NAO linka a lib
# SmileEngine. Ver o cabecalho do main.cpp para o que cada caminho medido isola.
#
# O benchmark continua standalone e compila sua propria instancia da biblioteca; ele nao
# linka SmileEngine, para medir os nove caminhos de alocacao sem inicializar o editor.

add_executable(SmileAllocBench
    main.cpp
    ${CMAKE_SOURCE_DIR}/Engine/ThirdParty/D3D12MA/D3D12MemAlloc.cpp
)

target_compile_features(SmileAllocBench PRIVATE cxx_std_20)
target_include_directories(SmileAllocBench PRIVATE ${CMAKE_SOURCE_DIR}/Engine/ThirdParty)
target_link_libraries(SmileAllocBench PRIVATE d3d12 dxgi)

# Codigo de terceiro nao passa pelo /W4 /permissive- da casa: o aviso que ele gera nao e
# acionavel (nao se corrige fonte vendorado) e afogaria o aviso do codigo que e nosso.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Engine/ThirdParty/D3D12MA/D3D12MemAlloc.cpp
    PROPERTIES COMPILE_OPTIONS "/W0"
)

set_target_properties(SmileAllocBench PROPERTIES
    FOLDER "Tools"
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
