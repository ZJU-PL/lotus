add_library(CanaryProfile STATIC
    Hot.cpp
)

target_link_libraries(CanaryProfile
    CanaryLoopStructure
    LLVMAnalysis
)
