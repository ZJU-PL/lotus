add_executable(llvm-ai llvm-ai.cpp)

target_link_libraries(llvm-ai
  IFDS
  CanaryDyckAA
  CanarySupport
  LLVMIRReader
)

set_property(TARGET llvm-ai PROPERTY CXX_STANDARD 17)
set_property(TARGET llvm-ai PROPERTY CXX_STANDARD_REQUIRED ON)
