/** @file LoopNestingGraph.h @brief Loop nesting graph for analyzing loop hierarchy and transitions. */
/*
 * Copyright 2026 Lotus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "IR/PDG/Construction/PDGCallGraph.h"

namespace lotus {
namespace analysis {
namespace loop {

class LoopNestingGraphInstructionNode;
class LoopNestingGraphLoopNode;
class LoopNestingGraphEdge;
class LoopNestingGraphLoopLoopEdge;

class LoopNestingGraphInstructionNode {
public:
  explicit LoopNestingGraphInstructionNode(CallBase *instruction)
      : instruction{instruction} {}

  CallBase *getInstruction(void) const { return this->instruction; }

private:
  CallBase *instruction;
};

class LoopNestingGraphLoopNode {
public:
  explicit LoopNestingGraphLoopNode(LoopStructure *loop) : loop{loop} {}

  LoopStructure *getLoop(void) const { return this->loop; }
  std::set<LoopNestingGraphEdge *> getIncomingEdges(void) const;
  std::set<LoopNestingGraphEdge *> getOutgoingEdges(void) const;
  LoopNestingGraphLoopLoopEdge *getNestingEdgeTo(
      LoopNestingGraphLoopNode *target) const;

private:
  friend class LoopNestingGraph;

  void addIncomingEdge(LoopNestingGraphEdge *edge);
  void addOutgoingEdge(LoopNestingGraphEdge *edge);

  LoopStructure *loop;
  std::set<LoopNestingGraphEdge *> incomingEdges;
  std::set<LoopNestingGraphEdge *> outgoingEdges;
};

class LoopNestingGraphEdge {
public:
  virtual ~LoopNestingGraphEdge() = default;

  bool isMust(void) const { return this->must; }

protected:
  explicit LoopNestingGraphEdge(bool must) : must{must} {}
  bool must;
};

class LoopNestingGraphInstructionLoopEdge : public LoopNestingGraphEdge {
public:
  LoopNestingGraphInstructionLoopEdge(LoopNestingGraphInstructionNode *instruction,
                                      LoopNestingGraphLoopNode *loop,
                                      bool must)
      : LoopNestingGraphEdge(must), instruction{instruction}, loop{loop} {}

  LoopNestingGraphInstructionNode *getInstructionNode(void) const {
    return this->instruction;
  }
  LoopNestingGraphLoopNode *getLoopNode(void) const { return this->loop; }

private:
  LoopNestingGraphInstructionNode *instruction;
  LoopNestingGraphLoopNode *loop;
};

class LoopNestingGraphLoopLoopEdge : public LoopNestingGraphEdge {
public:
  LoopNestingGraphLoopLoopEdge(LoopNestingGraphLoopNode *from,
                               LoopNestingGraphLoopNode *to,
                               bool must)
      : LoopNestingGraphEdge(must), from{from}, to{to} {}

  LoopNestingGraphLoopNode *getSrc(void) const { return this->from; }
  LoopNestingGraphLoopNode *getDst(void) const { return this->to; }
  void addSubEdge(LoopNestingGraphInstructionLoopEdge *edge);
  void setMust(void) { this->must = true; }
  std::vector<LoopNestingGraphInstructionLoopEdge *> getSubEdges(void) const;

private:
  LoopNestingGraphLoopNode *from;
  LoopNestingGraphLoopNode *to;
  std::vector<LoopNestingGraphInstructionLoopEdge *> subEdges;
};

class LoopNestingGraph {
public:
  explicit LoopNestingGraph(std::vector<LoopStructure *> const &loops);

  std::unordered_set<LoopNestingGraphLoopNode *> getLoopNodes(void) const;
  std::unordered_set<LoopNestingGraphEdge *> getEdges(void) const;
  LoopNestingGraphLoopNode *getLoopNode(LoopStructure *loop) const;
  LoopNestingGraphLoopNode *getEntryNode(Function *entryFunction) const;

  void createEdge(LoopStructure *from,
                  CallBase *callInst,
                  LoopStructure *child,
                  bool isMust);

  static std::unique_ptr<LoopNestingGraph>
  buildFromAnalyses(std::vector<FunctionLoopAnalyses *> const &analyses,
                    llvm::Module &module,
                    Function *entryFunction);

private:
  std::unordered_map<LoopStructure *, LoopNestingGraphLoopNode *> loops;
  std::unordered_map<Instruction *, LoopNestingGraphInstructionNode *>
      instructionNodes;
  std::map<LoopNestingGraphLoopNode *, std::set<LoopNestingGraphEdge *>> edges;

  LoopNestingGraphLoopLoopEdge *fetchOrCreateEdge(
      LoopNestingGraphLoopNode *fromNode,
      CallBase *callInst,
      LoopStructure *child,
      bool isMust);
};

} // namespace loop
} // namespace analysis
} // namespace lotus

