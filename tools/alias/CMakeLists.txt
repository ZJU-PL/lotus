# Alias analysis tools

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Coroutines
  Target
  BitWriter
)

# OriginAA Pointer Analysis Tool
add_executable(origin_aa origin_aa.cpp)
target_link_libraries(origin_aa PRIVATE
  OriginAA
  CanarySupport
  ${LLVM_LINK_COMPONENTS}
)

# Sea-DSA Memory Graph Analysis Tool
add_executable(sea-dsa-dg sea-dsa-dg.cpp)
target_link_libraries(sea-dsa-dg PRIVATE
  SeaDsaAnalysis
)

# Sea-DSA Advanced Analysis Tool
add_executable(seadsa-tool seadsa-tool.cpp)
target_link_libraries(seadsa-tool PRIVATE
  SeaDsaAnalysis
)

# FPA (Flow-sensitive Pointer Analysis) Tool
add_executable(fpa fpa.cpp)
target_link_libraries(fpa PRIVATE
  CanaryFPA
  ${LLVM_LINK_COMPONENTS}
)