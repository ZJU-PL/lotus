add_library(CanaryConditionalDyck STATIC
    ConditionalDyck.cpp
    ConCRA.cpp
)
target_include_directories(CanaryConditionalDyck PUBLIC
    ${PROJECT_SOURCE_DIR}/include)
target_compile_features(CanaryConditionalDyck PUBLIC cxx_std_17)
