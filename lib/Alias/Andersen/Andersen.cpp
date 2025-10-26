
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include "Alias/Andersen/Andersen.h"

#define DEBUG_TYPE "andersen"

using namespace llvm;

STATISTIC(NumValueNodes, "Number of value nodes created");
STATISTIC(NumConstraints, "Number of constraints collected");
STATISTIC(NumAddrOfConstraints, "Number of addr-of constraints");
STATISTIC(NumCopyConstraints, "Number of copy constraints");
STATISTIC(NumLoadConstraints, "Number of load constraints");
STATISTIC(NumStoreConstraints, "Number of store constraints");

cl::opt<bool> DumpDebugInfo("dump-debug",
                            cl::desc("Dump debug info into stderr"),
                            cl::init(false), cl::Hidden);
cl::opt<bool> DumpResultInfo("dump-result",
                             cl::desc("Dump result info into stderr"),
                             cl::init(false), cl::Hidden);
cl::opt<bool> DumpConstraintInfo("dump-cons",
                                 cl::desc("Dump constraint info into stderr"),
                                 cl::init(false), cl::Hidden);

Andersen::Andersen(const Module &module) { runOnModule(module); }

void Andersen::getAllAllocationSites(
    std::vector<const llvm::Value *> &allocSites) const {
  nodeFactory.getAllocSites(allocSites);
}

bool Andersen::getPointsToSet(const llvm::Value *v,
                              std::vector<const llvm::Value *> &ptsSet) const {
  NodeIndex ptrIndex = nodeFactory.getValueNodeFor(v);
  // We have no idea what v is...
  if (ptrIndex == AndersNodeFactory::InvalidIndex ||
      ptrIndex == nodeFactory.getUniversalPtrNode())
    return false;

  NodeIndex ptrTgt = nodeFactory.getMergeTarget(ptrIndex);
  ptsSet.clear();

  auto ptsItr = ptsGraph.find(ptrTgt);
  if (ptsItr == ptsGraph.end()) {
    // Can't find ptrTgt. The reason might be that ptrTgt is an undefined
    // pointer. Dereferencing it is undefined behavior anyway, so we might just
    // want to treat it as a nullptr pointer
    return true;
  }
  for (auto v : ptsItr->second) {
    if (v == nodeFactory.getNullObjectNode())
      continue;

    const llvm::Value *val = nodeFactory.getValueForNode(v);
    if (val != nullptr)
      ptsSet.push_back(val);
  }
  return true;
}

bool Andersen::runOnModule(const Module &M) {
  collectConstraints(M);
  
  // Update statistics after constraint collection
  NumConstraints = constraints.size();
  NumValueNodes = nodeFactory.getNumNodes();
  for (const auto &c : constraints) {
    switch (c.getType()) {
    case AndersConstraint::ADDR_OF:
      ++NumAddrOfConstraints;
      break;
    case AndersConstraint::COPY:
      ++NumCopyConstraints;
      break;
    case AndersConstraint::LOAD:
      ++NumLoadConstraints;
      break;
    case AndersConstraint::STORE:
      ++NumStoreConstraints;
      break;
    }
  }

  if (DumpDebugInfo)
    dumpConstraintsPlainVanilla();

  optimizeConstraints();

  if (DumpConstraintInfo)
    dumpConstraints();

  solveConstraints();

  if (DumpDebugInfo) {
    errs() << "\n";
    dumpPtsGraphPlainVanilla();
  }

  if (DumpResultInfo) {
    nodeFactory.dumpNodeInfo();
    errs() << "\n";
    dumpPtsGraphPlainVanilla();
  }

  return false;
}

void Andersen::dumpConstraint(const AndersConstraint &item) const {
  NodeIndex dest = item.getDest();
  NodeIndex src = item.getSrc();

  switch (item.getType()) {
  case AndersConstraint::COPY: {
    nodeFactory.dumpNode(dest);
    errs() << " = ";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::LOAD: {
    nodeFactory.dumpNode(dest);
    errs() << " = *";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::STORE: {
    errs() << "*";
    nodeFactory.dumpNode(dest);
    errs() << " = ";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::ADDR_OF: {
    nodeFactory.dumpNode(dest);
    errs() << " = &";
    nodeFactory.dumpNode(src);
  }
  }

  errs() << "\n";
}

void Andersen::dumpConstraints() const {
  errs() << "\n----- Constraints -----\n";
  for (auto const &item : constraints)
    dumpConstraint(item);
  errs() << "----- End of Print -----\n";
}

void Andersen::dumpConstraintsPlainVanilla() const {
  for (auto const &item : constraints) {
    errs() << item.getType() << " " << item.getDest() << " " << item.getSrc()
           << " 0\n";
  }
}

void Andersen::dumpPtsGraphPlainVanilla() const {
  for (unsigned i = 0, e = nodeFactory.getNumNodes(); i < e; ++i) {
    NodeIndex rep = nodeFactory.getMergeTarget(i);
    auto ptsItr = ptsGraph.find(rep);
    if (ptsItr != ptsGraph.end()) {
      errs() << i << " ";
      for (auto v : ptsItr->second)
        errs() << v << " ";
      errs() << "\n";
    }
  }
}
