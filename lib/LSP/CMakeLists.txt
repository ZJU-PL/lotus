add_library(CanaryLSP STATIC
  LSPServer.cpp
)

target_link_libraries(CanaryLSP PUBLIC
  LLVMCore
  LLVMIRReader
  LLVMSupport
)

