set(SPARTA_SOURCES
  LLVM_IFDS/IFDSFramework.cpp
  LLVM_IFDS/TaintAnalysis.cpp
  LLVM_IFDS/ReachingDefinitions.cpp
)

add_library(SpartaLLVM STATIC ${SPARTA_SOURCES})

# Link against LLVM libraries and Dyck alias analysis
target_link_libraries(SpartaLLVM
  LLVMSupport
  LLVMIRReader
  LLVMAnalysis
  CanaryDyckAA
  CanarySupport
)

# Set C++ standard
set_property(TARGET SpartaLLVM PROPERTY CXX_STANDARD 17)
set_property(TARGET SpartaLLVM PROPERTY CXX_STANDARD_REQUIRED ON)
