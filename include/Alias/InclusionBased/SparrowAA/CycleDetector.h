/**
 * @file CycleDetector.h
 * @brief Generic SCC-based cycle detector for Andersen's constraint graph.
 *
 * Implements Nuutila's improved Tarjan SCC algorithm iteratively with dense
 * arrays indexed by NodeIndex for minimal overhead.
 */

#pragma once

#include "Alias/InclusionBased/SparrowAA/GraphTraits.h"
#include "Alias/InclusionBased/SparrowAA/NodeFactory.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

template <class GraphType> class CycleDetector {
public:
  using GraphTraits = AndersGraphTraits<GraphType>;
  using NodeType = typename GraphTraits::NodeType;
  using node_iterator = typename GraphTraits::NodeIterator;
  using child_iterator = typename GraphTraits::ChildIterator;

private:
  /// Stack of nodes whose SCC membership has not yet been finalised.
  std::vector<const NodeType *> sccStack;

  /// Dense arrays indexed by NodeIndex (0 = unvisited; timestamps start at 1).
  std::vector<unsigned> dfsNum;
  std::vector<unsigned> entryTime;
  std::vector<uint8_t> inComponent;

  /// Nodes touched in the current run (enables O(|visited|) reset in runOnNode).
  std::vector<NodeIndex> visitedNodes;

  /// Monotonically increasing DFS timestamp counter (starts at 1).
  unsigned timestamp;

  struct WorkItem {
    NodeType *node;
    NodeIndex id;
    child_iterator childIt;
    child_iterator childEnd;
  };

  void ensureCapacity(NodeIndex id) {
    if (id < dfsNum.size())
      return;
    size_t newSize = std::max<size_t>(id + 1, dfsNum.size() * 2 + 64);
    dfsNum.resize(newSize, 0);
    entryTime.resize(newSize, 0);
    inComponent.resize(newSize, 0);
  }

  void visit(NodeType *startNode) {
    std::vector<WorkItem> worklist;

    auto pushNode = [&](NodeType *node) {
      NodeIndex id = node->getNodeIndex();
      ensureCapacity(id);
      unsigned ts = timestamp++;
      dfsNum[id] = ts;
      entryTime[id] = ts;
      visitedNodes.push_back(id);
      worklist.push_back({node, id, GraphTraits::child_begin(node),
                          GraphTraits::child_end(node)});
    };

    pushNode(startNode);

    while (!worklist.empty()) {
      WorkItem &item = worklist.back();
      NodeType *node = item.node;
      NodeIndex id = item.id;

      if (item.childIt != item.childEnd) {
        NodeType *succRep = getRep(*item.childIt);
        ++item.childIt;
        NodeIndex succId = succRep->getNodeIndex();
        ensureCapacity(succId);

        if (dfsNum[succId] == 0) {
          pushNode(succRep);
        } else {
          if (!inComponent[succId] && dfsNum[id] > dfsNum[succId])
            dfsNum[id] = dfsNum[succId];
        }
      } else {
        worklist.pop_back();

        if (!worklist.empty()) {
          NodeIndex parentId = worklist.back().id;
          if (!inComponent[id] && dfsNum[parentId] > dfsNum[id])
            dfsNum[parentId] = dfsNum[id];
        }

        unsigned myEntry = entryTime[id];
        if (dfsNum[id] != myEntry) {
          sccStack.push_back(node);
          continue;
        }

        inComponent[id] = 1;
        while (!sccStack.empty()) {
          const NodeType *cycleNode = sccStack.back();
          NodeIndex cId = cycleNode->getNodeIndex();
          if (dfsNum[cId] < myEntry)
            break;
          processNodeOnCycle(cycleNode, node);
          inComponent[cId] = 1;
          sccStack.pop_back();
        }

        processCycleRepNode(node);
      }
    }
  }

protected:
  virtual NodeType *getRep(NodeIndex node) = 0;
  virtual void processNodeOnCycle(const NodeType *node,
                                  const NodeType *repNode) = 0;
  virtual void processCycleRepNode(const NodeType *node) = 0;

  void reserveSCCState(size_t numNodes) {
    if (numNodes > dfsNum.size()) {
      dfsNum.resize(numNodes, 0);
      entryTime.resize(numNodes, 0);
      inComponent.resize(numNodes, 0);
    }
  }

  void runOnNodeList(const std::vector<NodeIndex> &nodeIndices) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");

    for (NodeIndex idx : nodeIndices) {
      NodeType *repNode = getRep(idx);
      NodeIndex repId = repNode->getNodeIndex();
      ensureCapacity(repId);
      if (dfsNum[repId] == 0)
        visit(repNode);
    }

    assert(sccStack.empty() && "sccStack not empty after cycle detection!");
  }

  void runOnGraph(GraphType *graph) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");

    std::vector<NodeIndex> initialNodes;
    for (auto itr = GraphTraits::node_begin(graph),
              ite = GraphTraits::node_end(graph);
         itr != ite; ++itr) {
      initialNodes.push_back(itr->getNodeIndex());
    }

    runOnNodeList(initialNodes);
  }

  void resetVisitedState() {
    for (NodeIndex id : visitedNodes) {
      dfsNum[id] = 0;
      entryTime[id] = 0;
      inComponent[id] = 0;
    }
    visitedNodes.clear();
    timestamp = 1;
  }

  void runOnNode(NodeIndex node) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");

    resetVisitedState();

    NodeType *repNode = getRep(node);
    NodeIndex repId = repNode->getNodeIndex();
    ensureCapacity(repId);
    visit(repNode);

    assert(sccStack.empty() && "sccStack not empty after cycle detection!");
  }

  void runOnNodeBatched(NodeIndex node) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");

    NodeType *repNode = getRep(node);
    NodeIndex repId = repNode->getNodeIndex();
    ensureCapacity(repId);
    if (dfsNum[repId] == 0)
      visit(repNode);

    assert(sccStack.empty() && "sccStack not empty after cycle detection!");
  }

  void releaseSCCMemory() {
    dfsNum.clear();
    entryTime.clear();
    inComponent.clear();
    visitedNodes.clear();
    sccStack.clear();
  }

public:
  CycleDetector() : timestamp(1) {}
  virtual ~CycleDetector() = default;

  virtual void run() = 0;
};
