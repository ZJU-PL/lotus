/** @file LoopStructure.h @brief Loop CFG structure descriptor for loop analyses. */
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

#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace llvm {
class BasicBlock;
class Function;
class Instruction;
class Loop;
class Value;
class raw_ostream;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class LoopStructure {
public:
  explicit LoopStructure(llvm::Loop *loop);

  uint64_t getID(void) const;
  void setID(uint64_t id);
  bool doesHaveID(void) const;

  llvm::Function *getFunction(void) const;
  llvm::BasicBlock *getHeader(void) const;
  llvm::BasicBlock *getPreHeader(void) const;
  llvm::Instruction *getEntryInstruction(void) const;
  llvm::BasicBlock *getSuccessorWithinLoopOfTheHeader(void) const;

  /// Return the nesting level of the loop (1 means outermost loop).
  uint32_t getNestingLevel(void) const;

  const std::unordered_set<llvm::BasicBlock *> &getLatches(void) const;
  const std::unordered_set<llvm::BasicBlock *> &getBasicBlocks(void) const;
  std::unordered_set<llvm::Instruction *> getInstructions(void) const;
  uint64_t getNumberOfInstructions(void) const;

  const std::vector<llvm::BasicBlock *> &getLoopExitBasicBlocks(void) const;
  bool isALoopExit(llvm::Instruction *instruction) const;
  const std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> &
  getLoopExitEdges(void) const;
  uint64_t numberOfExitBasicBlocks(void) const;

  bool isLoopInvariant(llvm::Value *value) const;
  bool isIncluded(llvm::BasicBlock *bb) const;
  bool isIncluded(llvm::Instruction *instruction) const;

  void print(llvm::raw_ostream &stream) const;

private:
  llvm::BasicBlock *header{nullptr};
  llvm::BasicBlock *preHeader{nullptr};
  uint32_t depth{0};
  std::unordered_set<llvm::Instruction *> invariants;
  std::unordered_set<llvm::BasicBlock *> latchBBs;
  std::unordered_set<llvm::BasicBlock *> bbs;
  std::vector<llvm::BasicBlock *> exitBlocks;
  std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> exitEdges;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
