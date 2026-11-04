#include "Analysis/CallGraph/VTA/TypePropagator.h"
#include "Analysis/CallGraph/VTA/TypeAssignmentGraph.h"

#include "llvm/IR/Function.h"

namespace lotus::vta {

static void initialize(TypeAssignment &TA, const TypeAssignmentGraph &TAG,
                       const SCCHolder &SCCs) {
  for (const auto &[Node, Types] : TAG.TypeEntryPoints) {
    if (Node < SCCs.SCCOfNode.size()) {
      auto SCC = SCCs.SCCOfNode[Node];
      if (SCC < TA.TypesPerSCC.size()) {
        TA.TypesPerSCC[SCC].insert(Types.begin(), Types.end());
      }
    }
  }
}

static void propagate(TypeAssignment &TA,
                      const SCCDependencyGraph &Deps,
                      uint32_t CurrSCC) {
  if (CurrSCC >= TA.TypesPerSCC.size()) {
    return;
  }
  const auto &Types = TA.TypesPerSCC[CurrSCC];
  if (Types.empty() || CurrSCC >= Deps.ChildrenOfSCC.size()) {
    return;
  }

  for (auto Succ : Deps.ChildrenOfSCC[CurrSCC]) {
    if (Succ < TA.TypesPerSCC.size()) {
      TA.TypesPerSCC[Succ].insert(Types.begin(), Types.end());
    }
  }
}

TypeAssignment propagateTypes(const TypeAssignmentGraph &TAG,
                              const SCCHolder &SCCs,
                              const SCCDependencyGraph &Deps,
                              const SCCOrder &Order) {
  TypeAssignment Ret;
  Ret.TypesPerSCC.resize(SCCs.size());

  initialize(Ret, TAG, SCCs);
  for (auto SCC : Order.SCCIds) {
    propagate(Ret, Deps, SCC);
  }

  return Ret;
}

void TypeAssignment::print(llvm::raw_ostream &OS,
                           const TypeAssignmentGraph &TAG,
                           const SCCHolder &SCCs) {
  OS << "digraph TypeAssignment {\n";
  for (size_t Ctr = 0; Ctr < TypesPerSCC.size(); ++Ctr) {
    OS << "  SCC " << Ctr << ": { ";
    for (auto Ty : TypesPerSCC[Ctr]) {
      if (const auto *Fun = Ty.dyn_cast<const llvm::Function *>()) {
        OS << "fun:" << Fun->getName() << " ";
      } else if (const auto *DITy = Ty.dyn_cast<const llvm::DIType *>()) {
        OS << "type:" << DITy->getName() << " ";
      }
    }
    OS << "}\n";
  }
  OS << "}\n";
}

} // namespace lotus::vta
