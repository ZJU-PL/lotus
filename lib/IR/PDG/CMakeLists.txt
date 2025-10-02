# Program Dependency Graph (PDG) CMake configuration
#
# This file configures the build for the PDG module, which implements
# a comprehensive program dependency graph for LLVM-based analysis.
# 
# The PDG system provides:
# - Field-sensitive program dependence analysis
# - Context-insensitive interprocedural analysis (?)
# - Support for both control and data dependencies
# - Structural analysis for C programs

add_library(CanaryPDG STATIC
  CallWrapper.cpp
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
  Tree.cpp
) 