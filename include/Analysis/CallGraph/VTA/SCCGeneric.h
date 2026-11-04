#pragma once

#include "Analysis/CallGraph/VTA/TypeAssignmentGraph.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace lotus::vta {

struct SCCHolder {
  std::vector<uint32_t> SCCOfNode;
  std::vector<llvm::SmallVector<uint32_t, 1>> NodesInSCC;

  [[nodiscard]] size_t size() const noexcept { return NodesInSCC.size(); }
  [[nodiscard]] bool empty() const noexcept { return NodesInSCC.empty(); }
};

struct SCCDependencyGraph {
  std::vector<llvm::SmallDenseSet<uint32_t>> ChildrenOfSCC;
  std::vector<uint32_t> SCCRoots;
};

struct SCCOrder {
  std::vector<uint32_t> SCCIds;
};

inline SCCHolder computeSCCs(const TypeAssignmentGraph &TAG) {
  SCCHolder Holder;
  uint32_t N = TAG.Nodes.size();
  if (N == 0) {
    return Holder;
  }

  Holder.SCCOfNode.resize(N, UINT32_MAX);
  std::vector<uint32_t> DfsIndex(N, 0);
  std::vector<uint32_t> LowLink(N, 0);
  std::vector<bool> OnStack(N, false);
  std::vector<uint32_t> Stack;
  uint32_t Index = 1;

  auto StrongConnect = [&](auto &Self, uint32_t V) -> void {
    DfsIndex[V] = Index;
    LowLink[V] = Index;
    ++Index;
    Stack.push_back(V);
    OnStack[V] = true;

    if (V < TAG.Adj.size()) {
      for (auto W : TAG.Adj[V]) {
        if (DfsIndex[W] == 0) {
          Self(Self, W);
          LowLink[V] = std::min(LowLink[V], LowLink[W]);
        } else if (OnStack[W]) {
          LowLink[V] = std::min(LowLink[V], DfsIndex[W]);
        }
      }
    }

    if (LowLink[V] == DfsIndex[V]) {
      uint32_t NewSCCId = Holder.NodesInSCC.size();
      auto &Nodes = Holder.NodesInSCC.emplace_back();

      while (true) {
        uint32_t W = Stack.back();
        Stack.pop_back();
        OnStack[W] = false;
        Holder.SCCOfNode[W] = NewSCCId;
        Nodes.push_back(W);
        if (W == V) {
          break;
        }
      }
    }
  };

  for (uint32_t I = 0; I < N; ++I) {
    if (DfsIndex[I] == 0) {
      StrongConnect(StrongConnect, I);
    }
  }

  return Holder;
}

inline SCCDependencyGraph
computeSCCDependencies(const TypeAssignmentGraph &TAG, const SCCHolder &SCCs) {
  SCCDependencyGraph Deps;
  Deps.ChildrenOfSCC.resize(SCCs.size());

  llvm::DenseSet<uint32_t> NonRoots;

  for (uint32_t U = 0; U < TAG.Nodes.size(); ++U) {
    uint32_t SrcSCC = SCCs.SCCOfNode[U];
    if (U >= TAG.Adj.size()) {
      continue;
    }
    for (auto V : TAG.Adj[U]) {
      uint32_t DstSCC = SCCs.SCCOfNode[V];
      if (SrcSCC != DstSCC) {
        if (Deps.ChildrenOfSCC[SrcSCC].insert(DstSCC).second) {
          NonRoots.insert(DstSCC);
        }
      }
    }
  }

  for (uint32_t I = 0; I < SCCs.size(); ++I) {
    if (!NonRoots.count(I)) {
      Deps.SCCRoots.push_back(I);
    }
  }

  return Deps;
}

inline SCCOrder computeSCCOrder(const SCCHolder &SCCs,
                               const SCCDependencyGraph &Deps) {
  SCCOrder Order;
  Order.SCCIds.reserve(SCCs.size());
  std::vector<bool> Visited(SCCs.size(), false);

  auto Dfs = [&](auto &Self, uint32_t SCC) -> void {
    Visited[SCC] = true;
    for (auto Succ : Deps.ChildrenOfSCC[SCC]) {
      if (!Visited[Succ]) {
        Self(Self, Succ);
      }
    }
    Order.SCCIds.push_back(SCC);
  };

  for (auto Root : Deps.SCCRoots) {
    if (!Visited[Root]) {
      Dfs(Dfs, Root);
    }
  }

  for (uint32_t I = 0; I < SCCs.size(); ++I) {
    if (!Visited[I]) {
      Dfs(Dfs, I);
    }
  }

  std::reverse(Order.SCCIds.begin(), Order.SCCIds.end());
  return Order;
}

inline std::pair<SCCHolder, SCCOrder>
computeSCCsAndTopologicalOrder(const TypeAssignmentGraph &TAG) {
  auto SCCs = computeSCCs(TAG);
  auto Deps = computeSCCDependencies(TAG, SCCs);
  auto Order = computeSCCOrder(SCCs, Deps);
  return {std::move(SCCs), std::move(Order)};
}

} // namespace lotus::vta
