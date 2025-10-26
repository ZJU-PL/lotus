add_library(CanaryAliasAnalysisWrapper STATIC
    AliasAnalysisWrapper.cpp
)

target_link_libraries(CanaryAliasAnalysisWrapper
    PUBLIC
        Andersen
        CanaryDyckAA
        CFLAA
        SeaDsaAnalysis
        CanaryAllocAA
        CanaryUnderApproxAA
)
