add_library(CanaryReport STATIC
    SARIF.cpp
    DebugInfoAnalysis.cpp
)
target_link_libraries(CanaryReport CanaryLLVMUtils)