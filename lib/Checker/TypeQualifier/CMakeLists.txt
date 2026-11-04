llvm_map_components_to_libnames(typequalifier_llvm_libs
  analysis
  #core
  #support
)

add_library(TypeQualifier STATIC
  AAMap.cpp
  Annotation.cpp
  CallGraph.cpp
  ConstraintCollect.cpp
  FuncProcess.cpp
  Helper.cpp
  NodeFactory.cpp
  QualifierAnalysis.cpp
  QualifierCheck.cpp
  QualiInference.cpp
  StructAnalyzer.cpp
)

target_link_libraries(TypeQualifier PRIVATE
  ${typequalifier_llvm_libs}
  CanaryFormats
  AliasSpecManager
)
