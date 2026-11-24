/**
 * @file TransformQuery.h
 * @brief Dependence-aware transform legality and scheduling queries over the PDG.
 */

#pragma once

#include "IR/PDG/Query/Criteria.h"

namespace llvm {
class DominatorTree;
class PostDominatorTree;
class LoopInfo;
class MemorySSA;
} // namespace llvm

namespace pdg {

/// Optional LLVM analyses used by transform-oriented PDG queries.
struct LLVMQueryContext {
  llvm::Function *function = nullptr;
  llvm::DominatorTree *dominator_tree = nullptr;
  llvm::PostDominatorTree *post_dominator_tree = nullptr;
  llvm::LoopInfo *loop_info = nullptr;
  llvm::MemorySSA *memory_ssa = nullptr;
};

struct MotionCheckResult {
  bool legal = false;
  Node *moving_node = nullptr;
  Node *anchor_node = nullptr;
  std::string reason;
  std::vector<Node *> blocking_path;
  std::vector<EdgeType> blocking_edge_types;
  PDGQueryDiagnostics diagnostics;
};

struct IndependenceCheckResult {
  bool independent = false;
  std::vector<Node *> witness_path_ab;
  std::vector<EdgeType> witness_edge_types_ab;
  std::vector<Node *> witness_path_ba;
  std::vector<EdgeType> witness_edge_types_ba;
  PDGQueryDiagnostics diagnostics;
};

/// Dependence-aware transform legality and scheduling helpers.
class TransformQuery {
public:
  explicit TransformQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  MotionCheckResult canMoveEarlier(Node &moving_node, Node &anchor_node,
                                   const LLVMQueryContext &llvm_context,
                                   const PDGQueryOptions &options =
                                       PDGQueryOptions()) const;

  MotionCheckResult canMoveLater(Node &moving_node, Node &anchor_node,
                                 const LLVMQueryContext &llvm_context,
                                 const PDGQueryOptions &options =
                                     PDGQueryOptions()) const;

  IndependenceCheckResult independent(
      Node &a, Node &b, const LLVMQueryContext &llvm_context,
      const PDGQueryOptions &options = PDGQueryOptions()) const;

  PDGQueryResult readySet(const PDGQueryScope &scope,
                          const PDGQueryResult::NodeSet &scheduled,
                          const LLVMQueryContext &llvm_context,
                          const PDGQueryOptions &options =
                              PDGQueryOptions()) const;

  std::vector<PDGQueryResult::NodeSet>
  topologicalLevels(const PDGQueryScope &scope,
                    const LLVMQueryContext &llvm_context,
                    const PDGQueryOptions &options =
                        PDGQueryOptions()) const;

  std::vector<PDGQueryResult::NodeSet>
  stronglyConnectedComponents(const PDGQueryScope &scope,
                              const LLVMQueryContext &llvm_context,
                              const PDGQueryOptions &options =
                                  PDGQueryOptions()) const;

  size_t criticalPathLength(const PDGQueryScope &scope,
                            const LLVMQueryContext &llvm_context,
                            const PDGQueryOptions &options =
                                PDGQueryOptions()) const;

private:
  ProgramGraph &pdg_;
};

} // namespace pdg
