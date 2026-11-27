/**
 * @file ConstraintOptimize.cpp
 * @brief Constraint optimization phase using HVN and HU algorithms.
 *
 * This file implements constraint optimization algorithms (HVN: Hash-based
 * Value Numbering, and HU: HVN with deReference and Union) that reduce the
 * number of constraints by merging equivalent nodes and eliminating redundant
 * constraints. These optimizations significantly speed up the constraint
 * solving phase.
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/SparrowAA/Andersen.h"
#include "Alias/InclusionBased/SparrowAA/CycleDetector.h"
#include "Alias/InclusionBased/SparrowAA/Log.h"
#include "Alias/InclusionBased/SparrowAA/SparseBitVectorGraph.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SparseBitVector.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "andersen"

using namespace llvm;

STATISTIC(NumConstraintsBeforeOpt, "Number of constraints before optimization");
STATISTIC(NumConstraintsAfterOpt, "Number of constraints after optimization");
STATISTIC(NumConstraintsEliminated, "Number of constraints eliminated");
STATISTIC(NumNodesMergedInOpt, "Number of nodes merged during optimization");
STATISTIC(NumPointerEquivClasses, "Number of pointer equivalence classes");
STATISTIC(NumLoadToStoreOptimized, "Number of loads optimized to copies");
STATISTIC(NumStoreToStoreOptimized, "Number of stores optimized to copies");

// Use extern to reference the category defined in Andersen.cpp
extern cl::OptionCategory AndersenCategory;

// HVN: Hash-based Value Numbering
cl::opt<bool> EnableHVN("enable-hvn",
                        cl::desc("Enable the HVN constraint optimization"),
                        cl::cat(AndersenCategory));
// HU: HVN with deReference and Union
cl::opt<bool> EnableHU("enable-hu",
                       cl::desc("Enable the HU constraint optimization"),
                       cl::cat(AndersenCategory));
// Adaptive context sensitivity via HVN feedback
cl::opt<bool> EnableAdaptiveCS(
    "andersen-adaptive-cs",
    cl::desc("Enable adaptive context sensitivity: functions whose pointer "
             "formals receive pointer-inequivalent actuals from different "
             "callers are given full k-CFA depth; others are not deepened"),
    cl::init(false), cl::cat(AndersenCategory));
cl::opt<float> AdaptiveCSThreshold(
    "andersen-adaptive-cs-threshold",
    cl::desc("Context-need score threshold in [0,1] for adaptive CS "
             "(default 0.5): functions scoring >= threshold get full context "
             "depth; others are not deepened"),
    cl::init(0.5f), cl::cat(AndersenCategory));

namespace {

using OptBitVector = SparseBitVector<512>;

struct SparseBitVectorHash {
  std::size_t operator()(const OptBitVector &vec) const {
    // Use a polynomial rolling hash to avoid the high collision rate of
    // plain XOR (which is commutative and maps e.g. {1,2} and {3} to the
    // same value when 1^2==3).  The multiplier 2654435761u is the Knuth
    // multiplicative hash constant for 32-bit values.
    std::size_t ret = 0;
    for (auto const &idx : vec)
      ret = ret * 2654435761u + idx + 1;
    return ret;
  }
};

struct SparseBitVectorKeyEqual {
  bool operator()(const OptBitVector &lhs, const OptBitVector &rhs) const {
    return lhs == rhs;
  }
};

// There is something in common in HVN and HU. Put all the shared stuffs in the
// base class here
class ConstraintOptimizer : public CycleDetector<SparseBitVectorGraph> {
protected:
  std::vector<AndersConstraint> &constraints;
  AndersNodeFactory &nodeFactory;

  // The predecessor graph
  SparseBitVectorGraph predGraph;
  // Nodes that must be treated conservatively (i.e. never merge with others)
  // Note that REF nodes and ADR nodes are all automatically indirect nodes.
  // This set only keep track of indirect nodes that are not REF or ADR
  DenseSet<NodeIndex> indirectNodes;

  // Dense table mapping NodeIndex (in [0, 3*numNodes)) -> Pointer Equivalence
  // Class (0 = non-pointer / unassigned).
  std::vector<unsigned> peLabel;
  // Current pointer equivalence class number
  unsigned pointerEqClass;

  // Dense union-find representative table for cycles within this pass.
  // InvalidIndex means the node is its own representative.
  std::vector<NodeIndex> mergeTarget;
  std::vector<NodeIndex> mergedNodesList;

  // During variable substitution, we create unknowns to represent the unknown
  // value that is a dereference of a variable.  These nodes are known as "ref"
  // nodes (since they represent the value of dereferences) Return the node
  // index of the "ref node" (used to represent *n) of n. We won't actually
  // create that ref node. We cannot use the NodeIndex of that refNode to index
  // into nodeFactory
  NodeIndex getRefNodeIndex(NodeIndex n) const {
    assert(n < nodeFactory.getNumNodes());
    return n + nodeFactory.getNumNodes();
  }

  // Return the node index of the "adr node" (used to represent &n) of n. Only
  // addr-taken vars can be adr-ed We won't actually create that adr node. We
  // cannot use the NodeIndex of that adrNode to index into nodeFactory
  NodeIndex getAdrNodeIndex(NodeIndex n) const {
    assert(n < nodeFactory.getNumNodes());
    return n + 2 * nodeFactory.getNumNodes();
  }

  void buildPredecessorGraph() {
    for (auto const &c : constraints) {
      NodeIndex srcTgt = nodeFactory.getMergeTarget(c.getSrc());
      NodeIndex dstTgt = nodeFactory.getMergeTarget(c.getDest());
      switch (c.getType()) {
      case AndersConstraint::ADDR_OF: {
        indirectNodes.insert(srcTgt);
        // Dest = &src edge: keep the exact original object node c.getSrc() so
        // merging src's points-to equivalence class never conflates &src with
        // &srcTgt across HVN -> HU passes.
        predGraph.insertEdge(dstTgt, getAdrNodeIndex(c.getSrc()));
        // *Dest = src edge
        predGraph.insertEdge(getRefNodeIndex(dstTgt), srcTgt);
        break;
      }
      case AndersConstraint::LOAD: {
        // dest = *src edge
        predGraph.insertEdge(dstTgt, getRefNodeIndex(srcTgt));
        break;
      }
      case AndersConstraint::STORE: {
        // *dest = src edge
        predGraph.insertEdge(getRefNodeIndex(dstTgt), srcTgt);
        break;
      }
      case AndersConstraint::COPY: {
        // Dest = Src edge
        predGraph.insertEdge(dstTgt, srcTgt);
        // *Dest = *Src edge
        predGraph.insertEdge(getRefNodeIndex(dstTgt), getRefNodeIndex(srcTgt));
        break;
      }
      }
    }
  }

  void printPredecessorGraphNode(raw_ostream &os, NodeIndex n) const {
    if (n >= nodeFactory.getNumNodes() * 2)
      os << "<ADR> ";
    else if (n >= nodeFactory.getNumNodes())
      os << "<REF> ";
    os << "[Node " << n % nodeFactory.getNumNodes() << "]";
  }

  void dumpPredecessorGraph() const {
    LOG_DEBUG("\n----- Predecessor Graph -----");
    for (NodeIndex nodeIdx : predGraph.getActiveNodes()) {
      const SparseBitVectorGraphNode *sNode =
          predGraph.getNodeWithIndex(nodeIdx);
      if (!sNode)
        continue;
      std::string nodeStr;
      raw_string_ostream rso(nodeStr);
      printPredecessorGraphNode(rso, nodeIdx);
      std::string prefix = nodeStr + "  -->  ";
      std::vector<std::string> idxStrs;
      for (auto const &idx : *sNode) {
        std::string idxStr;
        raw_string_ostream idxRso(idxStr);
        printPredecessorGraphNode(idxRso, idx);
        idxRso.flush();
        idxStrs.push_back(idxStr);
      }
      std::string result = prefix;
      for (size_t i = 0; i < idxStrs.size(); ++i) {
        if (i > 0)
          result += ", ";
        result += idxStrs[i];
      }
      LOG_DEBUG("{}", result);
    }
    LOG_DEBUG("----- End of Print -----");
  }

  void writePredecessorGraphToFile() const {
    std::error_code errInfo;
    ToolOutputFile outFile("dots/pred.dot", errInfo, sys::fs::OF_Text);
    if (errInfo) {
      LOG_ERROR("Failed to open output file: {}", errInfo.message());
      return;
    }

    raw_fd_ostream &os = outFile.os();
    os << "digraph G {\n";
    std::deque<bool> hasLabel(nodeFactory.getNumNodes() * 3, false);
    for (NodeIndex nodeIdx : predGraph.getActiveNodes()) {
      const SparseBitVectorGraphNode *sNode =
          predGraph.getNodeWithIndex(nodeIdx);
      if (!sNode)
        continue;
      if (!hasLabel[nodeIdx]) {
        os << "\tnode" << nodeIdx << " [label = \"";
        printPredecessorGraphNode(os, nodeIdx);
        os << "\"]\n";
        hasLabel[nodeIdx] = true;
      }
      for (auto const &idx : *sNode) {
        if (!hasLabel[idx]) {
          os << "\tnode" << idx << " [label = \"";
          printPredecessorGraphNode(os, idx);
          os << "\"]\n";
          hasLabel[idx] = true;
        }
        os << "\tnode" << idx << " -> "
           << "node" << nodeIdx << '\n';
      }
    }
    os << "}\n";

    outFile.keep();
  }

  NodeIndex getMergeTargetRep(NodeIndex idx) {
    NodeIndex root = idx;
    while (mergeTarget[root] != AndersNodeFactory::InvalidIndex)
      root = mergeTarget[root];
    while (idx != root) {
      NodeIndex next = mergeTarget[idx];
      mergeTarget[idx] = root;
      idx = next;
    }
    return root;
  }

  NodeType *getRep(NodeIndex idx) override {
    return predGraph.getOrInsertNode(getMergeTargetRep(idx));
  }
  // Specify how to process the non-rep nodes if a cycle is found
  void processNodeOnCycle(const NodeType *node,
                          const NodeType *repNode) override {
    NodeIndex nodeIdx = node->getNodeIndex();
    NodeIndex repIdx = repNode->getNodeIndex();
    NodeIndex repRoot = getMergeTargetRep(repIdx);
    if (mergeTarget[nodeIdx] == AndersNodeFactory::InvalidIndex)
      mergedNodesList.push_back(nodeIdx);
    mergeTarget[nodeIdx] = repRoot;
    if (indirectNodes.count(nodeIdx) || nodeIdx >= nodeFactory.getNumNodes())
      indirectNodes.insert(repIdx);

    predGraph.mergeEdge(repIdx, nodeIdx);
  }

  // Specify how to process the rep nodes if a cycle is found
  void processCycleRepNode(const NodeType *node) override {
    propagateLabel(node->getNodeIndex());
  }

  void rewriteConstraint() {
    const NodeIndex numNodes = nodeFactory.getNumNodes();
    std::vector<NodeIndex> revLabelMap(pointerEqClass,
                                       AndersNodeFactory::InvalidIndex);
    // Scan all the VAR nodes to see if any of them have the same label as other
    // VAR nodes. We have to perform the merge before constraint rewriting.
    for (NodeIndex node = 0; node < numNodes; ++node) {
      unsigned iLabel = peLabel[node];
      if (iLabel == 0)
        continue;
      if (nodeFactory.getMergeTarget(node) != node)
        continue;

      if (revLabelMap[iLabel] == AndersNodeFactory::InvalidIndex)
        revLabelMap[iLabel] = node;
      else {
        nodeFactory.mergeNode(revLabelMap[iLabel], node);
        ++NumNodesMergedInOpt;
      }
    }

    // Collect all peLabels that are assigned to ADR nodes.
    // Keep every ADR node even if its underlying varNode was merged into
    // another equivalence class: &varNode is still the distinct address of
    // varNode, while *(&varNode) resolves via getMergeTarget(varNode).
    for (NodeIndex node = numNodes * 2, e = numNodes * 3; node < e; ++node) {
      unsigned iLabel = peLabel[node];
      if (iLabel != 0)
        revLabelMap[iLabel] = node;
    }

    // Bucket rewritten constraints by type as packed (dest << 32 | src) 64-bit
    // keys for fast cache-friendly sort + deduplication.
    std::vector<uint64_t> byType[4];
    auto emitConstraint = [&](AndersConstraint::ConstraintType ty,
                              NodeIndex dst, NodeIndex src) {
      byType[static_cast<unsigned>(ty)].push_back(
          (static_cast<uint64_t>(dst) << 32) | static_cast<uint64_t>(src));
    };

    for (auto const &c : constraints) {
      // Change the lhs and rhs to their merge targets
      NodeIndex destTgt = nodeFactory.getMergeTarget(c.getDest());
      // First, if the lhs has label 0 (non-ptr), ignore this constraint
      if (peLabel[destTgt] == 0)
        continue;

      NodeIndex srcTgt = nodeFactory.getMergeTarget(c.getSrc());
      switch (c.getType()) {
      case AndersConstraint::ADDR_OF: {
        // We don't want to replace src with srcTgt because, after all, the
        // address of a variable is NOT the same as the address of another
        // variable
        emitConstraint(AndersConstraint::ADDR_OF, destTgt, c.getSrc());
        break;
      }
      case AndersConstraint::LOAD: {
        // If the src is a non-ptr, ignore this constraint
        if (peLabel[srcTgt] == 0)
          break;
        // If the rhs is equivalent to some ADR node (&X), replace load with
        // copy from getMergeTarget(X).
        NodeIndex srcTgtTgt = revLabelMap[peLabel[srcTgt]];
        if (srcTgtTgt != AndersNodeFactory::InvalidIndex &&
            srcTgtTgt >= numNodes * 2) {
          NodeIndex valRep = nodeFactory.getMergeTarget(srcTgtTgt - numNodes * 2);
          ++NumLoadToStoreOptimized;
          if (valRep != destTgt)
            emitConstraint(AndersConstraint::COPY, destTgt, valRep);
        } else {
          emitConstraint(AndersConstraint::LOAD, destTgt, srcTgt);
        }
        break;
      }
      case AndersConstraint::STORE: {
        // If the lhs is equivalent to some ADR node (&X), replace store with
        // copy into getMergeTarget(X).
        NodeIndex destTgtTgt = revLabelMap[peLabel[destTgt]];
        if (destTgtTgt != AndersNodeFactory::InvalidIndex &&
            destTgtTgt >= numNodes * 2) {
          NodeIndex valRep =
              nodeFactory.getMergeTarget(destTgtTgt - numNodes * 2);
          ++NumStoreToStoreOptimized;
          if (valRep != srcTgt)
            emitConstraint(AndersConstraint::COPY, valRep, srcTgt);
        } else {
          emitConstraint(AndersConstraint::STORE, destTgt, srcTgt);
        }
        break;
      }
      case AndersConstraint::COPY: {
        // Remove useless constraint "A=A"
        if (destTgt == srcTgt)
          break;

        // If the src is a non-ptr, ignore this constraint
        if (peLabel[srcTgt] == 0)
          break;

        // If the rhs is equivalent to some ADR node (&X), replace copy with
        // ADDR_OF(destTgt, X) using the original object node X.
        NodeIndex srcTgtTgt = revLabelMap[peLabel[srcTgt]];
        if (srcTgtTgt != AndersNodeFactory::InvalidIndex &&
            srcTgtTgt >= numNodes * 2) {
          NodeIndex origObj = srcTgtTgt - numNodes * 2;
          emitConstraint(AndersConstraint::ADDR_OF, destTgt, origObj);
        } else {
          emitConstraint(AndersConstraint::COPY, destTgt, srcTgt);
        }
        break;
      }
      }
    }

    size_t totalUnique = 0;
    for (unsigned t = 0; t < 4; ++t) {
      auto &vec = byType[t];
      std::sort(vec.begin(), vec.end());
      vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
      totalUnique += vec.size();
    }

    constraints.clear();
    constraints.reserve(totalUnique);
    for (unsigned t = 0; t < 4; ++t) {
      auto ty = static_cast<AndersConstraint::ConstraintType>(t);
      for (uint64_t packed : byType[t]) {
        constraints.emplace_back(ty, static_cast<NodeIndex>(packed >> 32),
                                 static_cast<NodeIndex>(packed & 0xFFFFFFFFu));
      }
    }
  }

  virtual void releaseMemory() {
    indirectNodes.clear();
    peLabel.clear();
    mergeTarget.clear();
    mergedNodesList.clear();
    predGraph.releaseMemory();
    releaseSCCMemory();
  }

  virtual void propagateLabel(NodeIndex node) = 0;

public:
  ConstraintOptimizer(std::vector<AndersConstraint> &c, AndersNodeFactory &n)
      : constraints(c), nodeFactory(n), predGraph(n.getNumNodes() * 3),
        peLabel(n.getNumNodes() * 3, 0), pointerEqClass(1),
        mergeTarget(n.getNumNodes() * 3, AndersNodeFactory::InvalidIndex) {
    reserveSCCState(n.getNumNodes() * 3);
    buildPredecessorGraph();
  }

  void run() override {
    // Run Tarjan's SCC algorithm only over active nodes in predGraph.
    runOnNodeList(predGraph.getActiveNodes());

    // For all nodes on the same cycle: assign their representative's pe label
    // to them
    for (NodeIndex nodeIdx : mergedNodesList)
      peLabel[nodeIdx] = peLabel[getMergeTargetRep(nodeIdx)];

    // Track pointer equivalence classes
    NumPointerEquivClasses = pointerEqClass;

    // We've done labelling. Now rewrite all constraints
    rewriteConstraint();
  }

  // Copy non-zero VAR peLabels into `out`.
  void copyPELabelsTo(DenseMap<NodeIndex, unsigned> &out) const {
    out.clear();
    const NodeIndex numNodes = nodeFactory.getNumNodes();
    for (NodeIndex i = 0; i < numNodes; ++i) {
      if (peLabel[i] != 0)
        out[i] = peLabel[i];
    }
  }
};

// The technique used here is described in "Exploiting Pointer and Location
// Equivalence to Optimize Pointer Analysis. In the 14th International Static
// Analysis Symposium (SAS), August 2007." It is known as the "HVN" algorithm,
// and is equivalent to value numbering the collapsed constraint graph without
// evaluating unions. This is used as a pre-pass to HU in order to resolve first
// order pointer dereferences and speed up/reduce memory usage of HU. Running
// both is equivalent to HRU without the iteration Since there are just too much
// bookkeeping info during HVN, we wrap all the logic of HVN into a single class
// here.
class HVNOptimizer : public ConstraintOptimizer {
private:
  // Map from a set of NodeIndex to Pointer Equivalence Class
  std::unordered_map<OptBitVector, unsigned, SparseBitVectorHash,
                     SparseBitVectorKeyEqual>
      setLabel;

  void propagateLabel(NodeIndex node) override {
    // Indirect node always gets a unique label
    if (node >= nodeFactory.getNumNodes() || indirectNodes.count(node)) {
      peLabel[node] = pointerEqClass++;
      return;
    }

    // Scan through the predecessor edges and examine what labels they have
    bool allSame = true;
    unsigned lastSeenLabel = 0;
    OptBitVector predLabels;
    const SparseBitVectorGraphNode *sNode = predGraph.getNodeWithIndex(node);
    if (sNode != nullptr) {
      for (auto const &pred : *sNode) {
        NodeIndex predRep = getMergeTargetRep(pred);
        unsigned predRepLabel = peLabel[predRep];
        // Ignore labels that are equal to us or non-pointers
        if (predRep == node || predRepLabel == 0)
          continue;

        if (lastSeenLabel == 0)
          lastSeenLabel = predRepLabel;
        else if (allSame && predRepLabel != lastSeenLabel)
          allSame = false;

        predLabels.set(predRepLabel);
      }
    }

    // We either have a non-pointer, a copy of an existing node, or a new node.
    // Assign the appropriate pointer equivalence label.
    if (predLabels.empty())
      peLabel[node] = 0;
    else if (allSame)
      peLabel[node] = lastSeenLabel;
    else {
      auto labelItr = setLabel.find(predLabels);
      if (labelItr != setLabel.end()) {
        peLabel[node] = labelItr->second;
      } else {
        setLabel.insert(std::make_pair(std::move(predLabels), pointerEqClass));
        peLabel[node] = pointerEqClass;
        ++pointerEqClass;
      }
    }
  }

public:
  HVNOptimizer(std::vector<AndersConstraint> &c, AndersNodeFactory &n)
      : ConstraintOptimizer(c, n) {}

  void releaseMemory() override {
    ConstraintOptimizer::releaseMemory();
    setLabel.clear();
  }
};

// The technique used here is described in "Exploiting Pointer and Location
// Equivalence to Optimize Pointer Analysis. In the 14th International Static
// Analysis Symposium (SAS), August 2007."  It is known as the "HU" algorithm,
// and is equivalent to value numbering the collapsed constraint graph including
// evaluating unions.
class HUOptimizer : public ConstraintOptimizer {
private:
  // Map from a set of NodeIndex to Pointer Equivalence Class
  std::unordered_map<OptBitVector, unsigned, SparseBitVectorHash,
                     SparseBitVectorKeyEqual>
      setLabel;
  // Dense table mapping NodeIndex -> offline pts-set
  std::vector<OptBitVector> ptsSet;

  // Try to assign a single label to node. Return true if the assignment
  // succeeds
  bool assignLabel(NodeIndex node) {
    // ADR nodes get a unique label and a pts-set that contains the
    // corresponding VAR node
    if (node >= nodeFactory.getNumNodes() * 2) {
      peLabel[node] = pointerEqClass++;
      ptsSet[node].set(node - nodeFactory.getNumNodes() * 2);
      return true;
    }

    // REF nodes get a unique label and a pts-set that contains itself (which
    // can never collide with VAR nodes)
    if (node >= nodeFactory.getNumNodes()) {
      peLabel[node] = pointerEqClass++;
      ptsSet[node].set(node);
      return true;
    }

    // Indirect VAR nodes get a unique label and a pts-set that contains its
    // corresponding ADR node (which can never collide with VAR and REF nodes)
    if (indirectNodes.count(node)) {
      peLabel[node] = pointerEqClass++;
      ptsSet[node].set(getAdrNodeIndex(node));
      return true;
    }

    return false;
  }

  void propagateLabel(NodeIndex node) override {
    if (assignLabel(node))
      return;

    // Direct VAR nodes need more careful examination
    OptBitVector &myPtsSet = ptsSet[node];
    SparseBitVectorGraphNode *sNode = predGraph.getNodeWithIndex(node);
    if (sNode != nullptr) {
      for (auto const &pred : *sNode) {
        NodeIndex predRep = getMergeTargetRep(pred);
        if (predRep != node && !ptsSet[predRep].empty())
          myPtsSet |= ptsSet[predRep];
      }
    }

    // If the ptsSet is empty, assign a label of zero
    if (myPtsSet.empty())
      peLabel[node] = 0;
    // Otherwise, see if we have seen this pattern before
    else {
      auto labelItr = setLabel.find(myPtsSet);
      if (labelItr != setLabel.end()) {
        peLabel[node] = labelItr->second;
      } else {
        setLabel.insert(std::make_pair(myPtsSet, pointerEqClass));
        peLabel[node] = pointerEqClass;
        ++pointerEqClass;
      }
    }
  }

public:
  HUOptimizer(std::vector<AndersConstraint> &c, AndersNodeFactory &n)
      : ConstraintOptimizer(c, n), ptsSet(n.getNumNodes() * 3) {}

  void releaseMemory() override {
    ConstraintOptimizer::releaseMemory();
    ptsSet.clear();
    setLabel.clear();
  }
};

} // end of anonymous namespace

// Run HVN, rewrite constraints, and copy the resulting peLabel map into `out`.
// Used by the adaptive context-sensitivity heuristic (see computeContextNeed).
void runHVNAndCapturePELabels(std::vector<AndersConstraint> &constraints,
                              AndersNodeFactory &nodeFactory,
                              llvm::DenseMap<NodeIndex, unsigned> &out) {
  HVNOptimizer hvn(constraints, nodeFactory);
  hvn.run();
  hvn.copyPELabelsTo(out);
}

// Optimize the constraints by performing offline variable substitution
void Andersen::optimizeConstraints() {
  // Track constraints before optimization
  NumConstraintsBeforeOpt = constraints.size();

  // First, let's do HVN.
  // When adaptive CS is active (k>0), HVN was already run during the CI
  // pre-pass in runOnModule and peLabel scores are already computed; skip the
  // duplicate run here unless the user also explicitly requested HVN.
  if (EnableHVN) {
    runHVNAndCapturePELabels(constraints, nodeFactory, hvnPELabels);
    // computeContextNeed() was already called in the pre-pass; no need to
    // re-run it here.
  }

  // Next, do HU
  // There is an additional assumption here that before HU, the predecessor
  // graph will have no cycle. Might fix that in the future
  if (EnableHU) {
    HUOptimizer hu(constraints, nodeFactory);
    hu.run();
  }

  // Track constraints after optimization
  NumConstraintsAfterOpt = constraints.size();
  NumConstraintsEliminated = NumConstraintsBeforeOpt - NumConstraintsAfterOpt;
}
