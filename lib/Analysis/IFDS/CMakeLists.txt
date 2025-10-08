# IFDS Framework - Independent Analysis Library

set(IFDS_SOURCES
  IFDSFramework.cpp
  IFDSDebugUtils.cpp
  IFDSTaintAnalysis.cpp
  IFDSReachingDefinitions.cpp
)

add_library(IFDS STATIC ${IFDS_SOURCES})

# Link with Checker library for taint configuration and Support library for progress bar
target_link_libraries(IFDS Annotation CanarySupport)

