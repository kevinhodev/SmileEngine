# SmileOmmBaker — bake offline dos Opacity Micro-Maps de uma cena cozida (<cena>.somm).
# Linka a lib SmileEngine, ao contrario do SmileCooker: o bake precisa do loader de cena e de
# textura do runtime, para enxergar o alpha exatamente como a GPU. Nao cria device D3D12.

add_executable(SmileOmmBaker
    main.cpp
)

target_compile_features(SmileOmmBaker PRIVATE cxx_std_20)
target_link_libraries(SmileOmmBaker PRIVATE SmileEngine)

set_target_properties(SmileOmmBaker PROPERTIES
    FOLDER "Tools"
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
