# Bug checking tools: gvfa, kint, taint, and concur

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Coroutines
  Target
)

# Global Value Flow Analysis Tool (unified, includes null pointer analysis)
add_executable(lotus-gvfa lotus_gvfa.cpp)
target_link_libraries(lotus-gvfa PRIVATE
  GVFA
  GVFAChecker
  CanaryReport
  Annotation
  CanaryDyckAA
  ${LLVM_LINK_COMPONENTS}
)

# Kint: A Bug-Finding Tool for C Programs
set(LLVM_LINK_COMPONENTS_KINT
        LLVMPasses
)

add_executable(lotus-kint lotus_kint.cpp)
target_link_libraries(lotus-kint PRIVATE
        CanaryKint
        CanaryReport
        CanarySupport
        ${LLVM_LINK_COMPONENTS_KINT}
        ${Z3_LIBRARIES} 
)

# IFDS Taint Analysis Tool (formerly llvm-ai)
add_executable(lotus-taint lotus_taint.cpp)
target_link_libraries(lotus-taint
  IFDS
  Annotation
  CanaryAliasAnalysisWrapper
)

# Concurrency Checker Tool
add_executable(lotus-concur lotus_concur.cpp)
target_link_libraries(lotus-concur
  ConcurrencyChecker
  CanaryReport
  #LLVMIRReader
  #LLVMAnalysis
)
