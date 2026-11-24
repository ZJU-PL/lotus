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
#include "Analysis/Loop/SCCDAGAttrs.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/NoelleGolden.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::GenericSCC;
using lotus::analysis::loop::LoopContent;
using lotus::analysis::loop::LoopStructure;
using lotus::analysis::loop::LoopTree;
using lotus::unittest::noelle_golden::combineOrderedValues;
using lotus::unittest::noelle_golden::combineUnorderedValues;
using lotus::unittest::noelle_golden::GoldenFile;
using lotus::unittest::noelle_golden::printAsOperandToString;
using lotus::unittest::noelle_golden::sectionMatches;
using lotus::unittest::noelle_golden::Values;
using lotus::unittest::noelle_golden::valueToString;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

static std::string llPath(const std::string &suite, const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_LL_DIR) + "/" + suite + "_" +
         name + ".ll";
}

static std::string goldenPath(const std::string &suite,
                              const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR) + "/" + suite + "/" +
         name + "/test.txt";
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

static std::vector<LoopContent *>
collectLoopContentsPreOrder(FunctionLoopAnalyses &analyses) {
  std::vector<LoopContent *> contents;
  auto *forest = analyses.getLoopForest();
  EXPECT_NE(forest, nullptr);
  if (forest == nullptr) {
    return contents;
  }

  auto trees = forest->getTrees();
  std::vector<LoopTree *> orderedTrees(trees.begin(), trees.end());
  std::sort(orderedTrees.begin(), orderedTrees.end(),
            [](LoopTree *lhs, LoopTree *rhs) {
              return printAsOperandToString(lhs->getLoop()->getHeader()) <
                     printAsOperandToString(rhs->getLoop()->getHeader());
            });

  for (auto *tree : orderedTrees) {
    tree->visitPreOrder([&](LoopTree *node, uint32_t) -> bool {
      auto *content = analyses.getLoopContent(node->getLoop()->getHeader());
      if (content != nullptr) {
        contents.push_back(content);
      }
      return false;
    });
  }

  return contents;
}

static Values collectStartAndStepByLoop(FunctionLoopAnalyses &analyses) {
  Values values;
  for (auto *content : collectLoopContentsPreOrder(analyses)) {
    auto *manager = content->getInductionVariableManager();
    auto ivs = manager->getInductionVariables(*content->getLoopStructure());
    for (auto *iv : ivs) {
      std::vector<std::string> parts;
      parts.push_back(
          printAsOperandToString(content->getLoopStructure()->getHeader()));
      parts.push_back(valueToString(iv->getStartValue()));
      parts.push_back(valueToString(iv->getSingleComputedStepValue()));
      values.insert(combineOrderedValues(parts));
    }
  }
  return values;
}

static Values collectLoopGoverning(FunctionLoopAnalyses &analyses) {
  Values values;
  for (auto *content : collectLoopContentsPreOrder(analyses)) {
    auto *manager = content->getInductionVariableManager();
    auto *giv = manager->getLoopGoverningInductionVariable(
        *content->getLoopStructure());
    if (giv == nullptr) {
      continue;
    }
    auto *iv = giv->getInductionVariable();
    std::vector<std::string> parts;
    parts.push_back(
        printAsOperandToString(content->getLoopStructure()->getHeader()));
    parts.push_back(combineOrderedValues(
        {valueToString(iv->getStartValue()),
         valueToString(iv->getSingleComputedStepValue())}));
    parts.push_back(valueToString(
        giv->getHeaderCompareInstructionToComputeExitCondition()));
    parts.push_back(valueToString(giv->getHeaderBrInst()));
    parts.push_back(valueToString(giv->getExitConditionValue()));
    for (auto *inst : giv->getConditionValueDerivation()) {
      parts.push_back(valueToString(inst));
    }
    values.insert(combineOrderedValues(parts));
  }
  return values;
}

static Values collectIntermediateValues(FunctionLoopAnalyses &analyses) {
  Values values;
  for (auto *content : collectLoopContentsPreOrder(analyses)) {
    auto *loop = content->getLoopStructure();
    values.insert(printAsOperandToString(loop->getHeader()));

    auto *manager = content->getInductionVariableManager();
    auto ivs = manager->getInductionVariables(*loop);
    std::vector<lotus::analysis::loop::InductionVariable *> orderedIVs(
        ivs.begin(), ivs.end());
    std::sort(orderedIVs.begin(), orderedIVs.end(), [](auto *lhs, auto *rhs) {
      return valueToString(lhs->getLoopEntryPHI()) <
             valueToString(rhs->getLoopEntryPHI());
    });

    for (auto *iv : orderedIVs) {
      std::vector<std::string> intermediates;
      auto instructions = iv->getAllInstructions();
      std::vector<llvm::Instruction *> orderedInstructions(instructions.begin(),
                                                           instructions.end());
      std::sort(orderedInstructions.begin(), orderedInstructions.end(),
                [](auto *lhs, auto *rhs) {
                  return valueToString(lhs) < valueToString(rhs);
                });
      for (auto *inst : orderedInstructions) {
        intermediates.push_back(valueToString(inst));
      }
      values.insert(combineUnorderedValues(intermediates));
    }
  }
  return values;
}

TEST(LoopParityIVTest, NestedLoopGoverningMatchesNoelleGolden) {
  llvm::LLVMContext context;
  auto module =
      llvm::parseIRFile(llPath("iv_attributes", "nested_loop_governing"),
                        *new llvm::SMDiagnostic(), context);
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

  GoldenFile golden(goldenPath("iv_attributes", "nested_loop_governing"));
  std::string diff;
  auto startAndStep = collectStartAndStepByLoop(analyses);
  EXPECT_TRUE(
      sectionMatches(golden, "verifyStartAndStepByLoop", startAndStep, &diff))
      << "Parity deviation for nested_loop_governing: " << diff;
  auto governing = collectLoopGoverning(analyses);
  EXPECT_TRUE(sectionMatches(golden, "verifyLoopGoverning", governing, &diff))
      << "Parity deviation for nested_loop_governing: " << diff;
}

TEST(LoopParityIVTest, NestedLoopIntermediateValuesMirrorNoelleCoverage) {
  llvm::LLVMContext context;
  auto module =
      llvm::parseIRFile(llPath("iv_attributes", "nested_loop_governing"),
                        *new llvm::SMDiagnostic(), context);
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

  auto contents = collectLoopContentsPreOrder(analyses);
  ASSERT_EQ(contents.size(), 3u);

  auto actual = collectIntermediateValues(analyses);
  ASSERT_EQ(actual.size(), 6u);

  for (auto *content : contents) {
    auto *loop = content->getLoopStructure();
    auto *manager = content->getInductionVariableManager();
    auto ivs = manager->getInductionVariables(*loop);
    ASSERT_EQ(ivs.size(), 1u)
        << "nested_loop_governing should expose one canonical IV per loop";

    auto *iv = *ivs.begin();
    auto instructions = iv->getAllInstructions();
    EXPECT_NE(actual.find(printAsOperandToString(loop->getHeader())),
              actual.end());
    EXPECT_NE(instructions.find(iv->getLoopEntryPHI()), instructions.end());

    auto *stepInstruction = llvm::dyn_cast_or_null<llvm::Instruction>(
        iv->getSingleComputedStepValue());
    if (stepInstruction != nullptr && loop->isIncluded(stepInstruction)) {
      EXPECT_NE(instructions.find(stepInstruction), instructions.end());
    } else if (iv->getSingleComputedStepValue() == nullptr) {
      auto stepComputation = iv->getComputationOfStepValue();
      EXPECT_FALSE(stepComputation.empty());
      for (auto *inst : stepComputation) {
        EXPECT_NE(instructions.find(inst), instructions.end());
      }
    }

    std::vector<std::string> intermediates;
    for (auto *inst : instructions) {
      intermediates.push_back(valueToString(inst));
    }
    EXPECT_NE(actual.find(combineUnorderedValues(intermediates)), actual.end());
  }
}

TEST(LoopParityIVTest, NestedLoopGoverningSCCsRemainLinearIVs) {
  llvm::LLVMContext context;
  auto module =
      llvm::parseIRFile(llPath("iv_attributes", "nested_loop_governing"),
                        *new llvm::SMDiagnostic(), context);
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
  analyses.materializeSCCAttrs(DT, PDT);

  auto contents = collectLoopContentsPreOrder(analyses);
  ASSERT_EQ(contents.size(), 3u);
  for (auto *content : contents) {
    auto *manager = content->getInductionVariableManager();
    auto *governing =
        manager->getLoopGoverningInductionVariable(*content->getLoopStructure());
    ASSERT_NE(governing, nullptr);

    auto *iv = governing->getInductionVariable();
    ASSERT_NE(iv, nullptr);
    auto *scc = content->getSCCDAG()->getSCC(iv->getLoopEntryPHI());
    ASSERT_NE(scc, nullptr);

    auto *attrs = content->getSCCAttrs();
    ASSERT_NE(attrs, nullptr);
    auto *info = attrs->getSCCAttrs(scc);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->getKind(), GenericSCC::LINEAR_INDUCTION_VARIABLE)
        << "governing IV SCC should remain classified as a linear IV for loop "
        << printAsOperandToString(content->getLoopStructure()->getHeader());
  }
}

TEST(LoopParityIVTest, IVSCCDAGDoesNotRetainExternalContextNodes) {
  llvm::LLVMContext context;
  auto module =
      llvm::parseIRFile(llPath("iv_attributes", "nested_loop_governing"),
                        *new llvm::SMDiagnostic(), context);
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

  auto contents = collectLoopContentsPreOrder(analyses);
  ASSERT_EQ(contents.size(), 3u);
  for (auto *content : contents) {
    auto *manager = content->getInductionVariableManager();
    auto ivs = manager->getInductionVariables(*content->getLoopStructure());
    ASSERT_FALSE(ivs.empty());
    for (auto *iv : ivs) {
      auto *scc = iv->getSCC();
      ASSERT_NE(scc, nullptr);
      for (auto &pair : scc->externalNodePairs()) {
        ASSERT_NE(pair.second, nullptr);
        EXPECT_TRUE(pair.second->isInternal())
            << "IV SCCDAG should not retain graph-external context nodes for loop "
            << printAsOperandToString(content->getLoopStructure()->getHeader());
      }
    }
  }
}

} // namespace
