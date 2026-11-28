/** @file FunctionLoopAnalyses.h @brief Driver for running multiple loop analyses on functions. */
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

#include "Analysis/Loop/LoopContent.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace llvm {
class AnalysisUsage;
class BasicBlock;
class DominatorTree;
class Function;
class Loop;
class LoopInfo;
class PostDominatorTree;
class ScalarEvolution;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class FunctionLoopAnalyses {
public:
  FunctionLoopAnalyses() = default;
  FunctionLoopAnalyses(llvm::Function &F,
                       llvm::LoopInfo &LI,
                       llvm::DominatorTree &DT,
                       llvm::PostDominatorTree &PDT);
  ~FunctionLoopAnalyses();

  FunctionLoopAnalyses(FunctionLoopAnalyses &&) noexcept = default;
  FunctionLoopAnalyses &operator=(FunctionLoopAnalyses &&) noexcept = default;

  FunctionLoopAnalyses(const FunctionLoopAnalyses &) = delete;
  FunctionLoopAnalyses &operator=(const FunctionLoopAnalyses &) = delete;

  llvm::Function *getFunction(void) const;

  LoopForest *getLoopForest(void) const;

  std::vector<LoopStructure *> getLoopStructures(void) const;
  std::vector<LoopContent *> getLoopContents(void) const;

  void materializeDependenceGraphs(pdg::ProgramGraph &pdg);
  void materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                 llvm::LoopInfo &LI);
  void materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                 llvm::LoopInfo &LI,
                                 const LoopDependenceBuilderOptions &options);
  void materializeLoopEnvironments(void);
  void materializeLoopCarriedDependencies(llvm::DominatorTree &DT,
                                          llvm::PostDominatorTree &PDT);
  void materializeIterationSpaceAnalyses(llvm::ScalarEvolution &SE);
  void materializeSCCAttrs(llvm::DominatorTree &DT,
                           llvm::PostDominatorTree &PDT,
                           bool enableFloatAsReal = false);

  LoopContent *getLoopContent(llvm::Loop &loop) const;
  LoopContent *getLoopContent(llvm::BasicBlock *header) const;

private:
  llvm::Function *function{nullptr};
  llvm::DominatorTree *dominatorTree{nullptr};
  llvm::PostDominatorTree *postDominatorTree{nullptr};
  std::vector<std::unique_ptr<LoopStructure>> ownedLoopStructures;
  std::unordered_map<llvm::BasicBlock *, LoopStructure *> loopByHeader;
  std::unique_ptr<LoopForest> forest;
  std::vector<std::unique_ptr<LoopContent>> loopContents;
  std::unordered_map<llvm::BasicBlock *, LoopContent *> contentByHeader;
};

class FunctionLoopAnalysesPass
    : public llvm::AnalysisInfoMixin<FunctionLoopAnalysesPass> {
public:
  using Result = FunctionLoopAnalyses;

  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);

private:
  friend llvm::AnalysisInfoMixin<FunctionLoopAnalysesPass>;
  static llvm::AnalysisKey Key;
};

class FunctionLoopAnalysesWrapperPass : public llvm::FunctionPass {
public:
  static char ID;

  FunctionLoopAnalysesWrapperPass();

  bool runOnFunction(llvm::Function &F) override;
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  FunctionLoopAnalyses &getResult(void);
  const FunctionLoopAnalyses &getResult(void) const;

private:
  std::unique_ptr<FunctionLoopAnalyses> result;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
