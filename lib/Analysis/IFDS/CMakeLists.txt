# IFDS Framework - Independent Analysis Library

set(IFDS_SOURCES
  IFDSFramework.cpp
  TaintAnalysis.cpp
  ReachingDefinitions.cpp
  BoostExceptionStub.cpp
)

add_library(IFDS STATIC ${IFDS_SOURCES})

# Link with Checker library for taint configuration
target_link_libraries(IFDS Checker)

# Set C++ standard
set_property(TARGET IFDS PROPERTY CXX_STANDARD 17)
set_property(TARGET IFDS PROPERTY CXX_STANDARD_REQUIRED ON)
