/** @file LoopDependenceBuilder.h @brief Builder and refinement pipeline for loop dependence graphs. */
/*
 * Copyright 2026 Lotus contributors
 */
#pragma once

#include "Analysis/Loop/Dependence/LoopDependenceGraph.h"
#include "Analysis/Loop/Dependence/LoopSCCDAG.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class LoopInfo;
class ScalarEvolution;
} // namespace llvm

namespace lotus {
namespace alias {
class AliasSpecManager;
} // namespace alias
} // namespace lotus

namespace noelle {
class DominatorSummary;
} // namespace noelle

namespace lotus {
namespace analysis {
namespace loop {

class MemoryCloningAnalysis;

class LoopDependenceRefinementPass {
public:
  virtual ~LoopDependenceRefinementPass() = default;

  virtual void refine(LoopDependenceGraph &graph,
                      LoopTree *loopNode,
                      llvm::ScalarEvolution &SE,
                      llvm::LoopInfo &LI,
                      const noelle::DominatorSummary &DS) = 0;
};

struct LoopDependenceBuilderOptions {
  bool enableLoopAwareDependenceAnalyses{true};
  bool hasLoopAwareDependenceBackend{true};
  bool enableAffineIterationSpaceRefinement{true};
  bool enableMemoryCloningRefinement{true};
  bool enableThreadSafeLibraryRefinement{true};
  bool enableExtendedIVRecognition{false};
  bool assumePseudoRandomValueGeneratorsNonDeterministic{false};
  std::vector<LoopDependenceRefinementPass *> loopAwareRefinementPasses;

  void addLoopAwareRefinementPass(LoopDependenceRefinementPass *pass);
  void removeLoopAwareRefinementPass(LoopDependenceRefinementPass *pass);
};

class LoopDependenceBuilder {
public:
  struct GraphBundle {
    std::unique_ptr<LoopDependenceGraph> graph;
    std::unique_ptr<LoopSCCDAG> sccdag;
    std::unique_ptr<MemoryCloningAnalysis> memoryCloningAnalysis;
    std::unique_ptr<LoopDependenceGraph> ivGraph;
    std::unique_ptr<LoopSCCDAG> ivSCCDAG;
    std::vector<std::pair<std::string, std::string>> debugSnapshots;

    GraphBundle() = default;
    GraphBundle(GraphBundle &&) noexcept = default;
    GraphBundle &operator=(GraphBundle &&) noexcept = default;
    ~GraphBundle();
  };

  static GraphBundle buildBaseLoopDependenceGraph(LoopTree *loopNode,
                                                  pdg::ProgramGraph &pdg);

  static GraphBundle refineLoopDependenceGraph(
      std::unique_ptr<LoopDependenceGraph> graph,
      llvm::ScalarEvolution &SE,
      llvm::LoopInfo &LI,
      const noelle::DominatorSummary &DS,
      const LoopDependenceBuilderOptions &options = {});

  static std::unique_ptr<LoopDependenceGraph>
  createInternalSubgraph(const LoopDependenceGraph &graph);

  static std::unique_ptr<LoopSCCDAG>
  computeSCCDAGWithOnlyVariableAndControlDependences(
      const LoopDependenceGraph &graph);

  static lotus::alias::AliasSpecManager &getAliasSpecManager(void);

private:
  static void captureSnapshot(GraphBundle &bundle,
                              const std::string &phase,
                              const LoopDependenceGraph &graph);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
