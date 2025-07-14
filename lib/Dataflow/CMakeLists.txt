set(WPDS_SOURCES
  Mono/DataFlowAnalysis.cpp
  Mono/DataFlowEngine.cpp
  Mono/DataFlowResult.cpp
  WPDS/DataFlowFacts.cpp
  WPDS/GenKillTransformer.cpp
  WPDS/InterProceduralDataFlowEngine.cpp
)

set(CanaryNullPointer_SOURCES
    NullPointer/LocalNullCheckAnalysis.cpp
    NullPointer/NullCheckAnalysis.cpp
    NullPointer/NullEquivalenceAnalysis.cpp
    NullPointer/NullFlowAnalysis.cpp
    NullPointer/ContextSensitiveLocalNullCheckAnalysis.cpp
    NullPointer/ContextSensitiveNullCheckAnalysis.cpp
    NullPointer/ContextSensitiveNullFlowAnalysis.cpp
    NullPointer/AliasAnalysisAdapter.cpp
)

set(GVFA_SOURCES
    GVFA/GlobalValueFlowAnalysis.cpp
)

add_library(DataFlow STATIC ${WPDS_SOURCES}) 


add_library(CanaryNullPointer STATIC ${CanaryNullPointer_SOURCES}) 
add_library(GVFA STATIC ${GVFA_SOURCES})

# Link GVFA with required dependencies
target_link_libraries(GVFA CanaryDyckAA CanarySupport CanaryTaint) 