# Alias analysis tools

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Coroutines
  Target
)

# AserPTA Pointer Analysis Tool
add_executable(aser-aa aser-aa.cpp)
target_link_libraries(aser-aa PRIVATE
  AserPTA
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

# LotusAA Pointer Analysis Tool
add_executable(lotus-aa lotus-aa.cpp)
target_link_libraries(lotus-aa PRIVATE
  LotusAA
  ${LLVM_LINK_COMPONENTS}
)

# Andersen's Pointer Analysis Tool
add_executable(ander-aa ander-aa.cpp)
target_link_libraries(ander-aa PRIVATE
  Andersen
  ${LLVM_LINK_COMPONENTS}
)

# DyckAA Pointer Analysis Tool
add_executable(dyck-aa dyck-aa.cpp)
target_link_libraries(dyck-aa PRIVATE
  CanaryDyckAA
  ${LLVM_LINK_COMPONENTS}
)

# Dynamic alias analysis
option(BUILD_DYNAA "Build dynamic alias analyses" OFF)

# Dynamic Alias Analysis tools (only built if BUILD_DYNAA is enabled)
if(BUILD_DYNAA)
  add_subdirectory(dynaa)
endif()