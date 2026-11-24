add_library(CanaryParameterSummary STATIC
  ParameterEffectSummary.cpp
  ResourceTable.cpp)
target_link_libraries(CanaryParameterSummary
  PUBLIC
    AliasSpecManager
    CanaryICFG)
