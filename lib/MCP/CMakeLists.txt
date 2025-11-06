add_library(CanaryMCP STATIC
  MCPServer.cpp
)

target_link_libraries(CanaryMCP PUBLIC
  LLVMCore
  LLVMIRReader
  LLVMSupport
)

