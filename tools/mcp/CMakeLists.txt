add_executable(mcp
  mcp.cpp
)

target_link_libraries(mcp
  PRIVATE
  CanaryMCP
  LLVMIRReader
)

