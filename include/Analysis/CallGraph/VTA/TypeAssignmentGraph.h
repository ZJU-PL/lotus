#pragma once

#include "Analysis/CallGraph/CallGraph.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace llvm {
class Value;
class Module;
} // namespace llvm

namespace lotus {
class Resolver;
class LLVMVFTableProvider;

namespace vta {

using TAGNodeId = uint32_t;

struct Variable {
  const llvm::Value *Val = nullptr;
};

struct Field {
  const llvm::DIType *Base = nullptr;
  size_t ByteOffset = 0;
};

struct Return {
  const llvm::Function *Fun = nullptr;
};

struct TAGNode {
  std::variant<Variable, Field, Return> Label;
};

inline bool operator==(Variable L, Variable R) noexcept {
  return L.Val == R.Val;
}
inline bool operator==(Field L, Field R) noexcept {
  return L.Base == R.Base && L.ByteOffset == R.ByteOffset;
}
inline bool operator==(Return L, Return R) noexcept {
  return L.Fun == R.Fun;
}
inline bool operator==(const TAGNode &L, const TAGNode &R) noexcept {
  return L.Label == R.Label;
}

} // namespace vta
} // namespace lotus

namespace llvm {
template <> struct DenseMapInfo<lotus::vta::TAGNode> {
  using TAGNode = lotus::vta::TAGNode;
  using Variable = lotus::vta::Variable;
  using Field = lotus::vta::Field;
  using Return = lotus::vta::Return;

  inline static TAGNode getEmptyKey() noexcept {
    return {Variable{llvm::DenseMapInfo<const llvm::Value *>::getEmptyKey()}};
  }
  inline static TAGNode getTombstoneKey() noexcept {
    return {Variable{llvm::DenseMapInfo<const llvm::Value *>::getTombstoneKey()}};
  }
  inline static bool isEqual(const TAGNode &L, const TAGNode &R) noexcept {
    return L == R;
  }
  inline static auto getHashValue(const TAGNode &TN) noexcept {
    if (const auto *Var = std::get_if<Variable>(&TN.Label)) {
      return llvm::hash_combine(0, Var->Val);
    }
    if (const auto *Fld = std::get_if<Field>(&TN.Label)) {
      return llvm::hash_combine(1, Fld->Base, Fld->ByteOffset);
    }
    if (const auto *Ret = std::get_if<Return>(&TN.Label)) {
      return llvm::hash_combine(2, Ret->Fun);
    }
    return llvm::hash_code(0);
  }
};
} // namespace llvm

namespace lotus::vta {

using TypeInfoTy = llvm::PointerUnion<const llvm::Function *, const llvm::DIType *>;

struct TypeAssignmentGraph {
  std::vector<TAGNode> Nodes;
  llvm::DenseMap<TAGNode, TAGNodeId> NodeToId;
  std::vector<llvm::SmallVector<TAGNodeId, 4>> Adj;
  llvm::DenseMap<TAGNodeId, llvm::SmallDenseSet<TypeInfoTy>> TypeEntryPoints;

  std::optional<TAGNodeId> get(const TAGNode &N) const {
    auto It = NodeToId.find(N);
    if (It != NodeToId.end()) {
      return It->second;
    }
    return std::nullopt;
  }

  TAGNodeId getOrInsert(const TAGNode &N) {
    auto It = NodeToId.find(N);
    if (It != NodeToId.end()) {
      return It->second;
    }
    TAGNodeId Id = Nodes.size();
    Nodes.push_back(N);
    NodeToId[N] = Id;
    if (Adj.size() <= Id) {
      Adj.resize(Id + 1);
    }
    return Id;
  }

  void addEdge(TAGNodeId From, TAGNodeId To) {
    if (From < Adj.size()) {
      for (auto Existing : Adj[From]) {
        if (Existing == To) {
          return;
        }
      }
      Adj[From].push_back(To);
    }
  }

  void print(llvm::raw_ostream &OS) const;
};

void printNode(llvm::raw_ostream &OS, const TAGNode &TN);

TypeAssignmentGraph computeTypeAssignmentGraph(
    const llvm::Module &M, const LLVMVFTableProvider &VTP, Resolver &BaseRes);

} // namespace lotus::vta
