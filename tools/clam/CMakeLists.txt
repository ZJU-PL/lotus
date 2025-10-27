# CLAM Tools - Abstract Interpretation-based Analyzer for LLVM

# Common include directories for all CLAM tools
set(CLAM_TOOLS_INCLUDES
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/Alias
  ${CRAB_ROOT}/include
  ${LLVM_INCLUDE_DIRS}
)

# Common compile definitions
set(CLAM_TOOLS_DEFINITIONS
  -D__STDC_CONSTANT_MACROS
  -D__STDC_LIMIT_MACROS
)

# ============================================================================
# CLAM - Main analysis tool
# ============================================================================
add_executable(clam clam.cc)

target_link_libraries(clam 
  ClamOptimizer
  ClamLlvmPasses
  Crab
  SeaDsaAnalysis
)

target_include_directories(clam PRIVATE ${CLAM_TOOLS_INCLUDES})
target_compile_definitions(clam PRIVATE ${CLAM_TOOLS_DEFINITIONS})

set_target_properties(clam PROPERTIES
  CXX_STANDARD 14
  CXX_STANDARD_REQUIRED ON
)

install(TARGETS clam RUNTIME DESTINATION bin)

# ============================================================================
# CLAM-DIFF - Compare JSON outputs of two CLAM analyses
# ============================================================================
add_executable(clam-diff clam-diff.cc)

llvm_map_components_to_libnames(CLAM_DIFF_LLVM_LIBS
  support core
)

target_link_libraries(clam-diff 
  ClamAnalysis
  Crab
  ${CLAM_DIFF_LLVM_LIBS}
)

target_include_directories(clam-diff PRIVATE ${CLAM_TOOLS_INCLUDES})
target_compile_definitions(clam-diff PRIVATE ${CLAM_TOOLS_DEFINITIONS})


install(TARGETS clam-diff RUNTIME DESTINATION bin)

# ============================================================================
# CLAM-PP - LLVM bitcode Pre-Processor for static analysis
# ============================================================================
add_executable(clam-pp clam-pp.cc)

target_link_libraries(clam-pp 
  ClamLlvmPasses
  SeaDsaAnalysis
)

target_include_directories(clam-pp PRIVATE ${CLAM_TOOLS_INCLUDES})
target_compile_definitions(clam-pp PRIVATE ${CLAM_TOOLS_DEFINITIONS})

install(TARGETS clam-pp RUNTIME DESTINATION bin)

# ============================================================================
message(STATUS "CLAM tools configured: clam, clam-diff, clam-pp")

