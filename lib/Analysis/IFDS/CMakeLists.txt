# IFDS Framework - Independent Analysis Library

# Framework core
set(IFDS_FRAMEWORK_SOURCES
  IDESolver.cpp
  IFDSDebugUtils.cpp
  ParallelIFDSSolver.cpp
)

# Client analyses
set(IFDS_CLIENT_SOURCES
  Clients/IFDSTaintAnalysis.cpp
  Clients/IFDSReachingDefinitions.cpp
  Clients/IDEConstantPropagation.cpp
  Clients/IDETypeState.cpp
)

set(IFDS_SOURCES ${IFDS_FRAMEWORK_SOURCES} ${IFDS_CLIENT_SOURCES})

add_library(IFDS STATIC ${IFDS_SOURCES})

# Link with Checker library for taint configuration and Support library for progress bar
target_link_libraries(IFDS Annotation CanarySupport)

