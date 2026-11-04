set(CanaryTypeHierarchy_SOURCES
  TypeHierarchyAnalysis.cpp
  DIBasedTypeHierarchy.cpp
  DIBasedTypeHierarchyData.cpp
  LLVMVFTable.cpp
  LLVMVFTableData.cpp
  LLVMVFTableProvider.cpp
)
add_library(CanaryTypeHierarchy STATIC ${CanaryTypeHierarchy_SOURCES})
target_link_libraries(CanaryTypeHierarchy CanaryLLVMUtils CanaryCFG)
