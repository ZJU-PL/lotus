set(MONO_SOURCES
  DataFlowAnalysis.cpp
  DataFlowEngine.cpp
  DataFlowResult.cpp
  ReachableAnalysis.cpp
  LiveVariablesAnalysis.cpp
)
add_library(MONODataFlow STATIC ${MONO_SOURCES})

  
