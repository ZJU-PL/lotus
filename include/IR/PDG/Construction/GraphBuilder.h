#pragma once

#include "IR/PDG/Core/Graph.h"

namespace pdg {

/// Builds the structural PDG: instruction/global nodes, function and call
/// wrappers, and parameter trees. Dependence passes add edges afterward.
/// The graph owns the constructed objects; the LLVM module must outlive use
/// of its values through the graph. Building again replaces the prior graph.
class GraphBuilder {
public:
  explicit GraphBuilder(ProgramGraph &graph) : graph_(graph) {}
  void build(llvm::Module &module);
  void bindDebugInfo(llvm::Module &module);

private:
  llvm::DIType *computeNodeDIType(Node &node);
  bool isAnnotationCallInst(llvm::Instruction &instruction);
  void buildGlobalAnnotationNodes(llvm::Module &module);
  ProgramGraph &graph_;
};

} // namespace pdg
