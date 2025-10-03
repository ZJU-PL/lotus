# Program Dependency Graph (PDG) CMake configuration
#
# This file configures the build for the PDG module, which implements
# a comprehensive program dependency graph for LLVM-based analysis.
# 

add_library(CanaryPDG STATIC
  CallWrapper.cpp
  ContextSensitiveSlicing.cpp
  ControlDependencyGraph.cpp
  DataDependencyGraph.cpp
  DebugInfoUtils.cpp
  FunctionWrapper.cpp
  Graph.cpp
  GraphWriter.cpp
  PDGCallGraph.cpp
  PDGNode.cpp
  PDGUtils.cpp
  ProgramDependencyGraph.cpp
  QueryLanguage.cpp
  QueryParser.cpp
  Slicing.cpp
  Tree.cpp
) 