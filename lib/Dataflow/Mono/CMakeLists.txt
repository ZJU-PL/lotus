# Header-only framework components live under include/Dataflow/Mono/.
# MONODataFlow contains the compiled concrete analyses.

set(MONO_SOURCES
  Analyses/Intra/AvailableExpressions.cpp
  Analyses/Intra/ConstantPropagation.cpp
  Analyses/Intra/FullConstantPropagation.cpp
  Analyses/Intra/LiveVariables.cpp
  Analyses/Intra/Reachability.cpp
  Analyses/Intra/ReachingDefinitions.cpp
  Analyses/Intra/Taint.cpp
  Analyses/Intra/UninitializedVariables.cpp
  Analyses/Inter/AvailableExpressions.cpp
  Analyses/Inter/ConstantPropagation.cpp
  Analyses/Inter/FullConstantPropagation.cpp
  Analyses/Inter/LiveVariables.cpp
  Analyses/Inter/Reachability.cpp
  Analyses/Inter/ReachingDefinitions.cpp
  Analyses/Inter/Taint.cpp
  Analyses/Inter/UninitializedVariables.cpp
)

add_library(MONODataFlow STATIC ${MONO_SOURCES})
target_link_libraries(MONODataFlow PUBLIC CanaryAliasAnalysisWrapper)
