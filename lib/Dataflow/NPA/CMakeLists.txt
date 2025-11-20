# NPA sources (simplified structure - files moved to parent directory)
set(NPA_SOURCES
  BitVectorDomain.cpp
  BitVectorSolver.cpp
  ReachingDefinitions.cpp
  ReachableBlocks.cpp
)
add_library(NPADataFlow STATIC ${NPA_SOURCES})
