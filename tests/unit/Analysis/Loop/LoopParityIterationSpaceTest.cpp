#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/NoelleGolden.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::unittest::noelle_golden::combineUnorderedValues;
using lotus::unittest::noelle_golden::GoldenFile;
using lotus::unittest::noelle_golden::sectionMatches;
using lotus::unittest::noelle_golden::Values;
using lotus::unittest::noelle_golden::valueToString;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

constexpr const char *kDisjointBetweenIterationsSection =
    "verifyDisjointAccessBetweenIterations";
constexpr const char *kDisjointBetweenIterationsAfterSCEVSection =
    "verifyDisjointAccessBetweenIterationsAfterSCEVSimplification";

static std::string llPath(const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_LL_DIR) + "/loop_domain_space_" +
         name + ".ll";
}

static std::string goldenPath(const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR) +
         "/loop_domain_space/" + name + "/test.txt";
}

static void buildPDG(llvm::Module &module) {
  auto &registry = *llvm::PassRegistry::getPassRegistry();
  llvm::initializeCore(registry);
  llvm::initializeAnalysis(registry);
  llvm::initializeTransformUtils(registry);
  llvm::legacy::PassManager pm;
  pm.add(new DataDependencyGraph());
  pm.add(new ControlDependencyGraph());
  pm.add(new ProgramDependencyGraph());
  pm.run(module);
}

static Values collectDisjointAccesses(FunctionLoopAnalyses &analyses,
                                      llvm::LoopInfo &LI) {
  Values values;
  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  auto *iteration = content->getLoopIterationSpaceAnalysis();
  auto *ls = content->getLoopStructure();
  std::vector<Instruction *> accesses;
  for (auto *bb : ls->getBasicBlocks()) {
    for (auto &inst : *bb) {
      if (llvm::isa<llvm::LoadInst>(&inst) ||
          llvm::isa<llvm::StoreInst>(&inst)) {
        accesses.push_back(&inst);
      }
    }
  }

  for (auto *a : accesses) {
    for (auto *b : accesses) {
      if (a == b) {
        continue;
      }
      if (iteration
              ->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
                  a, b)) {
        values.insert(
            combineUnorderedValues({valueToString(a), valueToString(b)}));
      }
    }
  }
  return values;
}

static void runCase(
    const std::string &name,
    const std::string &requestedSection = kDisjointBetweenIterationsSection) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(llPath(name), diagnostic, context);
  ASSERT_NE(module, nullptr);
  buildPDG(*module);

  auto *function = module->getFunction("main");
  ASSERT_NE(function, nullptr);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(ProgramGraph::getInstance());
  analyses.materializeScalarAnalyses(SE, LI);
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);

  GoldenFile golden(goldenPath(name));
  auto section = requestedSection;
  if (!golden.hasSection(section) &&
      section == kDisjointBetweenIterationsAfterSCEVSection &&
      golden.hasSection(kDisjointBetweenIterationsSection)) {
    section = kDisjointBetweenIterationsSection;
  }

  std::string diff;
  auto actual = collectDisjointAccesses(analyses, LI);
  EXPECT_TRUE(sectionMatches(golden, section, actual, &diff))
      << "Parity deviation for loop_domain_space/" << name << ": "
      << diff;
}

TEST(LoopParityIterationSpaceTest, OneDimArrayMatchesNoelleGolden) {
  runCase("one_dim_array");
}

TEST(LoopParityIterationSpaceTest, TwoDimMatrixMatchesNoelleGolden) {
  runCase("two_dim_matrix");
}

TEST(LoopParityIterationSpaceTest,
     OneDimArrayAfterSCEVSimplificationMatchesImportedOracle) {
  runCase("one_dim_array", kDisjointBetweenIterationsAfterSCEVSection);
}

TEST(LoopParityIterationSpaceTest,
     TwoDimMatrixAfterSCEVSimplificationMatchesImportedOracle) {
  runCase("two_dim_matrix", kDisjointBetweenIterationsAfterSCEVSection);
}

} // namespace
