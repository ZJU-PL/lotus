add_library(CanaryMultiplicity STATIC MultiplicityClassifier.cpp)
target_link_libraries(CanaryMultiplicity
  PUBLIC
    AliasSpecManager)
