# FIXME: Indeed, the Mono dir is independent from WPDS
# Besides, the WPDS implementaton is very likely unfinished.
set(WPDS_SOURCES
  Mono/DataFlowAnalysis.cpp
  Mono/DataFlowEngine.cpp
  Mono/DataFlowResult.cpp
  WPDS/DataFlowFacts.cpp
  WPDS/GenKillTransformer.cpp
  WPDS/InterProceduralDataFlowEngine.cpp
)
add_library(DataFlow STATIC ${WPDS_SOURCES})

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
add_library(CanaryNullPointer STATIC ${CanaryNullPointer_SOURCES})

set(GVFA_SOURCES
  GVFA/GlobalValueFlowAnalysis.cpp
  GVFA/ReachabilityAlgorithms.cpp
  GVFA/VulnerabilityCheckers.cpp
)
add_library(GVFA STATIC ${GVFA_SOURCES})
target_link_libraries(GVFA CanaryDyckAA CanarySupport Annotation)

add_subdirectory(sparta)
add_subdirectory(IFDS)






