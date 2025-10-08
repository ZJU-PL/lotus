# IFDS Framework - Independent Analysis Library

# Framework core
set(IFDS_FRAMEWORK_SOURCES
  IFDSSolver.cpp
  IDESolver.cpp
  IFDSDebugUtils.cpp
)

# Client analyses
set(IFDS_CLIENT_SOURCES
  Clients/IFDSTaintAnalysis.cpp
  Clients/IFDSReachingDefinitions.cpp
)

set(IFDS_SOURCES ${IFDS_FRAMEWORK_SOURCES} ${IFDS_CLIENT_SOURCES})

add_library(IFDS STATIC ${IFDS_SOURCES})

# Link with Checker library for taint configuration and Support library for progress bar
target_link_libraries(IFDS Annotation CanarySupport)

