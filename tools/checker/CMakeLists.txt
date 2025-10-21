# Bug checking tools: canary, gvfa-tool, kint, and ifds-taint

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Coroutines
  Target
)

# Canary: Null Pointer Analysis Driver
add_executable(canary-npa canary_npa.cpp)
target_link_libraries(canary-npa PRIVATE
  CanaryNullPointer 
  CanaryDyckAA 
  CanaryTransform 
  CanaryLLVMUtils
  ${LLVM_LINK_COMPONENTS}
)

# Global Value Flow Analysis Tool
add_executable(canary-gvfa canary_gvfa.cpp)
target_link_libraries(canary-gvfa PRIVATE
  GVFA
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
  CanaryDyckAA
  CanaryLLVMUtils
  LLVMIRReader
)

# Concurrency Checker Tool
add_executable(concurrency-checker concurrency-checker.cpp)
target_link_libraries(concurrency-checker
  ConcurrencyChecker
  LLVMIRReader
  LLVMAnalysis
)
