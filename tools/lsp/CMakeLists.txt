add_executable(lsp
  lsp.cpp
)

target_link_libraries(lsp
  PRIVATE
  CanaryLSP
  LLVMIRReader
)

