add_executable(pdg-query
  pdg-query.cpp
)

target_link_libraries(pdg-query
  PRIVATE
  CanaryPDG
  LLVMIRReader
)
