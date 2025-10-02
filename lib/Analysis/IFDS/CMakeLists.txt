# IFDS Framework - Independent Analysis Library

set(IFDS_SOURCES
  IFDSFramework.cpp
  IFDSDebugUtils.cpp
  TaintAnalysis.cpp
  ReachingDefinitions.cpp
)

add_library(IFDS STATIC ${IFDS_SOURCES})

# Link with Checker library for taint configuration
target_link_libraries(IFDS Annotation)

# Set C++ standard
set_property(TARGET IFDS PROPERTY CXX_STANDARD 14)
set_property(TARGET IFDS PROPERTY CXX_STANDARD_REQUIRED ON)
