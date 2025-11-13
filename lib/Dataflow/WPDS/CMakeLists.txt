# WPDS-based interprocedural dataflow analysis implementation
set(WPDS_SOURCES
  DataFlowFacts.cpp
  GenKillTransformer.cpp
  InterProceduralDataFlowEngine.cpp
  WPDSUninitializedVariables.cpp
  WPDSConstantPropagation.cpp
  WPDSLivenessAnalysis.cpp
  WPDSTaintAnalysis.cpp
)
add_library(WPDSDataFlow STATIC ${WPDS_SOURCES})
