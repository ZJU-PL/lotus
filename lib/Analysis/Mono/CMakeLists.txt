set(MONO_SOURCES
  DataFlowAnalysis.cpp
  DataFlowEngine.cpp
  DataFlowResult.cpp
)
add_library(MONODataFlow STATIC ${MONO_SOURCES})

  
