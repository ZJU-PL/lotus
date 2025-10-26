# Bug checking tools: gvfa, kint, ifds-taint, and concurrency-checker

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Coroutines
  Target
)

# Global Value Flow Analysis Tool (unified, includes null pointer analysis)
add_executable(canary-gvfa canary_gvfa.cpp)
target_link_libraries(canary-gvfa PRIVATE
  GVFA
  GVFAChecker
  CanaryReport
  Annotation
  ${LLVM_LINK_COMPONENTS}
)

# Kint: A Bug-Finding Tool for C Programs
set(LLVM_LINK_COMPONENTS_KINT
        LLVMPasses
)

add_executable(kint kint.cpp)
target_link_libraries(kint PRIVATE
        CanaryKint
        CanaryReport
        CanarySupport
        ${LLVM_LINK_COMPONENTS_KINT}
        ${Z3_LIBRARIES} 
)

# IFDS Taint Analysis Tool (formerly llvm-ai)
add_executable(ifds-taint ifds_taint.cpp)
target_link_libraries(ifds-taint
  IFDS
  Annotation
  CanaryAliasAnalysisWrapper
  CanaryDyckAA
  CanaryLLVMUtils
  LLVMIRReader
)

# Concurrency Checker Tool
add_executable(concurrency-checker concurrency-checker.cpp)
target_link_libraries(concurrency-checker
  ConcurrencyChecker
  CanaryReport
  LLVMIRReader
  LLVMAnalysis
)
