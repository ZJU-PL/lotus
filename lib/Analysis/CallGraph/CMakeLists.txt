set(CanaryCallGraph_SOURCES
  CallGraphAnalysisType.cpp
  CallGraph.cpp
  CallGraphBuilder.cpp
  Resolver.cpp
  VirtualCallUtils.cpp
  AllocatedTypes.cpp
  CHA/CHAResolver.cpp
  RTA/RTAResolver.cpp
  VTA/VTAResolver.cpp
  VTA/TypeAssignmentGraph.cpp
  VTA/TypePropagator.cpp
  OTF/OTFResolver.cpp
)
add_library(CanaryCallGraph STATIC ${CanaryCallGraph_SOURCES})
target_link_libraries(CanaryCallGraph CanaryTypeHierarchy CanaryLLVMUtils CanaryCFG)

add_subdirectory(FPA)
