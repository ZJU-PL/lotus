#pragma once

#include "Analysis/CallGraph/VTA/SCCGeneric.h"
#include "Analysis/CallGraph/VTA/TypeAssignmentGraph.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace lotus::vta {

struct TypeAssignment {
  std::vector<llvm::SmallDenseSet<TypeInfoTy>> TypesPerSCC;

  void print(llvm::raw_ostream &OS, const TypeAssignmentGraph &TAG,
             const SCCHolder &SCCs);
};

TypeAssignment propagateTypes(const TypeAssignmentGraph &TAG,
                              const SCCHolder &SCCs,
                              const SCCDependencyGraph &Deps,
                              const SCCOrder &Order);

} // namespace lotus::vta
