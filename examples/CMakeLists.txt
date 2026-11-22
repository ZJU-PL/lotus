# Examples CMakeLists.txt

set(EXAMPLES_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

# Get LLVM components needed for examples that don't get LLVM through dependencies
llvm_map_components_to_libnames(llvm_libs irreader analysis)

# Set example binary output directory
set(LOTUS_EXAMPLE_BIN_DIR ${CMAKE_BINARY_DIR}/bin/examples)

function(lotus_add_example target_name source_file)
  add_executable(${target_name} ${source_file})
  set_target_properties(${target_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${LOTUS_EXAMPLE_BIN_DIR}
  )
  target_include_directories(${target_name} PUBLIC ${EXAMPLES_INCLUDE_DIR})
endfunction()

# PDGExample needs CanaryPDG, AliasAnalysisWrapper (for DataDependencyGraph), and LLVM libs
lotus_add_example(PDGExample ir/PDGExample.cpp)
target_link_libraries(PDGExample PRIVATE 
    CanaryPDG 
    CanaryAliasAnalysisWrapper 
    ${llvm_libs}
)

# PointerAnalysisExample needs AliasAnalysisWrapper and all its dependencies
lotus_add_example(PointerAnalysisExample alias/PointerAnalysisExample.cpp)
target_link_libraries(PointerAnalysisExample PRIVATE 
    CanaryAliasAnalysisWrapper 
    CanaryUnderApproxAA
    CanaryDyckAA
    CanaryAllocAA
    SeaDsaAnalysis
    ${llvm_libs}
)

if(TARGET CanaryCFLCertificates)
  add_subdirectory(cfl/certificates)
endif()
