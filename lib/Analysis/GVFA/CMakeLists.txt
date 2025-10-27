set(GVFA_SOURCES
  GlobalValueFlowAnalysis.cpp
  ReachabilityAlgorithms.cpp
  GVFAUtils.cpp
)
add_library(GVFA STATIC ${GVFA_SOURCES})
target_link_libraries(GVFA CanaryDyckAA)

