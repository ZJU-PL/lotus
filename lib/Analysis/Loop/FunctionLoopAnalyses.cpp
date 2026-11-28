/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/FunctionLoopAnalyses.h"

#include "Analysis/CFG/Dominator.h"
#include "Analysis/Loop/Dependence/LoopDependenceBuilder.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"

#include <cassert>

namespace lotus {
namespace analysis {
namespace loop {

FunctionLoopAnalyses::FunctionLoopAnalyses(llvm::Function &F,
                                           llvm::LoopInfo &LI,
                                           llvm::DominatorTree &DT,
                                           llvm::PostDominatorTree &PDT)
    : function{&F}, dominatorTree{&DT}, postDominatorTree{&PDT} {
  std::vector<LoopStructure *> loopPtrs;
  loopPtrs.reserve(LI.getLoopsInPreorder().size());

  for (auto *loop : LI.getLoopsInPreorder()) {
    auto owned = std::make_unique<LoopStructure>(loop);
    auto *raw = owned.get();
    this->loopByHeader[raw->getHeader()] = raw;
    this->ownedLoopStructures.push_back(std::move(owned));
    loopPtrs.push_back(raw);
  }

  if (loopPtrs.empty()) {
    return;
  }

  noelle::DominatorSummary summary(DT, PDT);
  std::unordered_map<llvm::Function *, noelle::DominatorSummary *> doms;
  doms[&F] = &summary;
  this->forest = std::make_unique<LoopForest>(loopPtrs, doms);

  for (auto *loopStructure : loopPtrs) {
    auto *loopNode = this->forest->getNode(loopStructure);
    if (loopNode == nullptr) {
      continue;
    }
    auto ownedContent = std::make_unique<LoopContent>(loopNode);
    auto *rawContent = ownedContent.get();
    this->contentByHeader[loopStructure->getHeader()] = rawContent;
    this->loopContents.push_back(std::move(ownedContent));
  }
}

FunctionLoopAnalyses::~FunctionLoopAnalyses() = default;

llvm::Function *FunctionLoopAnalyses::getFunction(void) const {
  return this->function;
}

LoopForest *FunctionLoopAnalyses::getLoopForest(void) const {
  return this->forest.get();
}

std::vector<LoopStructure *>
FunctionLoopAnalyses::getLoopStructures(void) const {
  std::vector<LoopStructure *> loops;
  loops.reserve(this->ownedLoopStructures.size());
  for (const auto &owned : this->ownedLoopStructures) {
    loops.push_back(owned.get());
  }
  return loops;
}

std::vector<LoopContent *> FunctionLoopAnalyses::getLoopContents(void) const {
  std::vector<LoopContent *> contents;
  contents.reserve(this->loopContents.size());
  for (const auto &owned : this->loopContents) {
    contents.push_back(owned.get());
  }
  return contents;
}

void FunctionLoopAnalyses::materializeDependenceGraphs(pdg::ProgramGraph &pdg) {
  for (const auto &ownedContent : this->loopContents) {
    ownedContent->materializeDependenceGraph(pdg);
  }
}

void FunctionLoopAnalyses::materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                                     llvm::LoopInfo &LI) {
  this->materializeScalarAnalyses(SE, LI, LoopDependenceBuilderOptions{});
}

void FunctionLoopAnalyses::materializeScalarAnalyses(
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    const LoopDependenceBuilderOptions &options) {
  assert(this->dominatorTree != nullptr);
  assert(this->postDominatorTree != nullptr);
  noelle::DominatorSummary summary(*this->dominatorTree,
                                   *this->postDominatorTree);
  for (const auto &ownedContent : this->loopContents) {
    ownedContent->materializeScalarAnalyses(SE, LI, summary, options);
    auto *llvmLoop = LI.getLoopFor(ownedContent->getLoopStructure()->getHeader());
    if (llvmLoop != nullptr) {
      ownedContent->setCompileTimeTripCount(
          SE.getSmallConstantTripCount(llvmLoop));
    }
  }
}

void FunctionLoopAnalyses::materializeLoopEnvironments(void) {
  for (const auto &ownedContent : this->loopContents) {
    ownedContent->materializeEnvironment();
  }
}

void FunctionLoopAnalyses::materializeLoopCarriedDependencies(
    llvm::DominatorTree &DT,
    llvm::PostDominatorTree &PDT) {
  noelle::DominatorSummary summary(DT, PDT);
  for (const auto &ownedContent : this->loopContents) {
    ownedContent->materializeLoopCarriedDependencies(summary);
  }
}

void FunctionLoopAnalyses::materializeIterationSpaceAnalyses(
    llvm::ScalarEvolution &SE) {
  for (const auto &ownedContent : this->loopContents) {
    ownedContent->materializeIterationSpaceAnalysis(SE);
  }
}

void FunctionLoopAnalyses::materializeSCCAttrs(llvm::DominatorTree &DT,
                                               llvm::PostDominatorTree &PDT,
                                               bool enableFloatAsReal) {
  noelle::DominatorSummary summary(DT, PDT);
  for (const auto &ownedContent : this->loopContents) {
    ownedContent->materializeSCCAttrs(enableFloatAsReal, summary);
  }
}

LoopContent *FunctionLoopAnalyses::getLoopContent(llvm::Loop &loop) const {
  return this->getLoopContent(loop.getHeader());
}

LoopContent *FunctionLoopAnalyses::getLoopContent(
    llvm::BasicBlock *header) const {
  auto it = this->contentByHeader.find(header);
  return it == this->contentByHeader.end() ? nullptr : it->second;
}

llvm::AnalysisKey FunctionLoopAnalysesPass::Key;

FunctionLoopAnalysesPass::Result
FunctionLoopAnalysesPass::run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(F);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(F);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(F);
  return FunctionLoopAnalyses(F, LI, DT, PDT);
}

char FunctionLoopAnalysesWrapperPass::ID = 0;

FunctionLoopAnalysesWrapperPass::FunctionLoopAnalysesWrapperPass()
    : llvm::FunctionPass(ID) {}

bool FunctionLoopAnalysesWrapperPass::runOnFunction(llvm::Function &F) {
  auto &LI = getAnalysis<llvm::LoopInfoWrapperPass>().getLoopInfo();
  auto &DT = getAnalysis<llvm::DominatorTreeWrapperPass>().getDomTree();
  auto &PDT = getAnalysis<llvm::PostDominatorTreeWrapperPass>().getPostDomTree();
  this->result = std::make_unique<FunctionLoopAnalyses>(F, LI, DT, PDT);
  return false;
}

void FunctionLoopAnalysesWrapperPass::getAnalysisUsage(
    llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<llvm::LoopInfoWrapperPass>();
  AU.addRequired<llvm::DominatorTreeWrapperPass>();
  AU.addRequired<llvm::PostDominatorTreeWrapperPass>();
}

FunctionLoopAnalyses &FunctionLoopAnalysesWrapperPass::getResult(void) {
  assert(this->result != nullptr);
  return *this->result;
}

const FunctionLoopAnalyses &
FunctionLoopAnalysesWrapperPass::getResult(void) const {
  assert(this->result != nullptr);
  return *this->result;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
