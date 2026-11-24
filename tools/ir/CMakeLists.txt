add_executable(lotus-ir-pdg-query
  lotus-ir-pdg-query.cpp
  pdg-query/Options.cpp
  pdg-query/Driver.cpp
  pdg-query/Output.cpp
)

target_link_libraries(lotus-ir-pdg-query
  PRIVATE
  CanaryPDGConstruction
  CanaryPDGRules
  CanaryPDGCypher
  CanaryCUDD
  CanaryAliasAnalysisWrapper
  LLVMIRReader
)

add_executable(lotus-ir-control-dependence
  lotus-ir-control-dependence.cpp
)

target_link_libraries(lotus-ir-control-dependence
  PRIVATE
  CanaryControlDependence
  LLVMIRReader
)

add_executable(lotus-ir-usetracessa lotus-ir-usetracessa.cpp)
target_link_libraries(lotus-ir-usetracessa PRIVATE CanaryUseTraceSSASVFG LLVMIRReader)

add_executable(lotus-ir-ufg lotus-ir-ufg.cpp)
target_link_libraries(lotus-ir-ufg PRIVATE CanaryUFG CanaryUseTraceSSASVFG LLVMIRReader)
