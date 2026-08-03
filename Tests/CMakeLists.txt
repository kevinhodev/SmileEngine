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
