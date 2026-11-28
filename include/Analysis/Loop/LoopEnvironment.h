/** @file LoopEnvironment.h @brief Loop boundary environment tracking live-in and live-out values. */
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

#include "Analysis/Loop/Dependence/LoopDependenceGraph.h"
#include "llvm/ADT/iterator_range.h"

#include <cstdint>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class BasicBlock;
class Instruction;
class Type;
class Value;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class LoopEnvironment {
public:
  LoopEnvironment(LoopDependenceGraph *loopDG,
                  std::vector<llvm::BasicBlock *> const &exitBlocks);
  LoopEnvironment(LoopDependenceGraph *loopDG,
                  std::vector<llvm::BasicBlock *> const &exitBlocks,
                  const std::set<llvm::Value *> &excludeValues);

  llvm::iterator_range<std::vector<llvm::Value *>::iterator> getProducers(void);
  llvm::iterator_range<std::set<int>::iterator> getEnvIDsOfLiveInVars(void) const;
  llvm::iterator_range<std::set<int>::iterator> getEnvIDsOfLiveOutVars(void) const;

  uint64_t size(void) const;
  uint64_t getNumberOfLiveIns(void) const;
  uint64_t getNumberOfLiveOuts(void) const;
  int64_t getExitBlockID(void) const;

  llvm::Type *typeOfEnvironmentLocation(uint64_t id) const;
  std::vector<llvm::Type *> getTypesOfEnvironmentLocations(void) const;

  bool isLiveIn(llvm::Value *val) const;
  bool isProducer(llvm::Value *producer) const;
  llvm::Value *getProducer(uint64_t id) const;
  std::set<llvm::Value *> consumersOf(llvm::Value *prod) const;

private:
  std::vector<llvm::Value *> envProducers;
  std::unordered_map<llvm::Value *, int> producerIDMap;
  std::set<int> liveInIDs;
  std::set<int> liveOutIDs;
  std::unordered_map<llvm::Value *, std::set<llvm::Value *>> prodConsumers;
  bool hasExitBlockEnv;
  llvm::Type *exitBlockType;

  uint64_t addProducer(llvm::Value *producer, bool liveIn);
  uint64_t addLiveInProducer(llvm::Value *producer);
  void addLiveOutProducer(llvm::Value *producer);
  uint64_t addLiveInValue(
      llvm::Value *newLiveInValue,
      const std::unordered_set<llvm::Instruction *> &consumers);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
