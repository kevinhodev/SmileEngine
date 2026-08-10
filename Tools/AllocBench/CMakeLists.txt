# SmileAllocBench — mede quanto do custo de criar recurso e o heap implicito.
# Console app standalone: cria o proprio device D3D12 e NAO linka a lib SmileEngine.
# Ver o cabecalho do main.cpp para o que cada caminho medido isola.

add_executable(SmileAllocBench main.cpp)

target_compile_features(SmileAllocBench PRIVATE cxx_std_20)
target_link_libraries(SmileAllocBench PRIVATE d3d12 dxgi)

set_target_properties(SmileAllocBench PROPERTIES
    FOLDER "Tools"
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
