#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"

#include "Analysis/Loop/Classification/SCCDAGAttrs.h"
#include "Analysis/Loop/Dependence/LoopDependenceGraph.h"
#include "Analysis/Loop/Dependence/LoopSCCDAG.h"
#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"
#include "TestUtils/NoelleGolden.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::GenericSCC;
using lotus::analysis::loop::LoopCarriedSCC;
using lotus::analysis::loop::LoopDependenceNode;
using lotus::analysis::loop::LoopSCC;
using lotus::unittest::loadModule;
using lotus::unittest::noelle_golden::combineOrderedValues;
using lotus::unittest::noelle_golden::combineUnorderedValues;
using lotus::unittest::noelle_golden::GoldenFile;
using lotus::unittest::noelle_golden::sectionMatches;
using lotus::unittest::noelle_golden::Values;
using lotus::unittest::noelle_golden::valueToString;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

static std::string renderNoelleLikeValue(llvm::Value *value) {
  if (value == nullptr) {
    return "";
  }

  if (auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
    if (ce->getOpcode() == llvm::Instruction::GetElementPtr) {
      std::string text = valueToString(ce);
      auto firstSpace = text.find(' ');
      if (firstSpace != std::string::npos) {
        text.erase(0, firstSpace + 1);
      }
      const std::string inbounds = "inbounds ";
      auto inboundsPos = text.find(inbounds);
      if (inboundsPos != std::string::npos) {
        text.erase(inboundsPos, inbounds.size());
      }
      auto gepPos = text.find("getelementptr (");
      if (gepPos != std::string::npos) {
        text.erase(gepPos + std::strlen("getelementptr "), 1);
        if (!text.empty() && text.back() == ')') {
          text.pop_back();
        }
      }
      return std::string("%ce = ") + text;
    }
  }

  if (auto *call = llvm::dyn_cast<llvm::CallBase>(value)) {
    std::string text = valueToString(call);
    auto gepPos = text.find("getelementptr");
    if (gepPos != std::string::npos) {
      auto argStart = text.rfind("i8* ", gepPos);
      auto argEnd = text.find(", i32", gepPos);
      if (argStart != std::string::npos && argEnd != std::string::npos) {
        text.replace(argStart, argEnd - argStart, "i8* %ce0");
      }
    }
    return text;
  }

  return valueToString(value);
}

static std::string llPath(const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_LL_DIR) + "/sccdag_attributes_" +
         name + ".ll";
}

static std::string goldenPath(const std::string &name) {
  return std::string(LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR) +
         "/sccdag_attributes/" + name + "/test.txt";
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

static std::vector<LoopSCC *> orderedSCCs(FunctionLoopAnalyses &analyses,
                                          llvm::LoopInfo &LI) {
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *sccdag = content->getSCCDAG();
  auto sccs = sccdag->getSCCs();
  std::sort(sccs.begin(), sccs.end(), [](LoopSCC *lhs, LoopSCC *rhs) {
    return lhs->getID() < rhs->getID();
  });
  return sccs;
}

static std::vector<LoopSCC *> orderedAllSCCs(FunctionLoopAnalyses &analyses,
                                             llvm::LoopInfo &LI) {
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *sccdag = content->getSCCDAG();
  auto sccs = sccdag->getAllSCCs();
  std::sort(sccs.begin(), sccs.end(), [](LoopSCC *lhs, LoopSCC *rhs) {
    return lhs->getID() < rhs->getID();
  });
  return sccs;
}

static Values collectSCCDAGNodes(FunctionLoopAnalyses &analyses,
                                 llvm::LoopInfo &LI) {
  Values values;
  std::set<std::string> boundaryValues;
  for (auto *scc : orderedAllSCCs(analyses, LI)) {
    std::vector<std::string> sccValues;
    for (auto &pair : scc->internalNodePairs()) {
      if (pair.first != nullptr) {
        sccValues.push_back(renderNoelleLikeValue(pair.first));
      }
    }
    values.insert(combineUnorderedValues(sccValues));

    for (auto &pair : scc->externalNodePairs()) {
      if (pair.first != nullptr) {
        boundaryValues.insert(renderNoelleLikeValue(pair.first));
      }
    }

    for (auto &pair : scc->internalNodePairs()) {
      auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(pair.first);
      if (call == nullptr) {
        continue;
      }
      auto *callee = call->getCalledFunction();
      if (callee == nullptr || callee->getName() != "printf" ||
          call->arg_size() == 0) {
        continue;
      }
      auto *formatArg = call->getArgOperand(0);
      if (llvm::isa<llvm::ConstantExpr>(formatArg)) {
        values.insert(renderNoelleLikeValue(formatArg));
      }
    }

    for (auto &pair : scc->externalNodePairs()) {
      auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(pair.first);
      if (call == nullptr) {
        continue;
      }
      auto *callee = call->getCalledFunction();
      if (callee == nullptr || callee->getName() != "printf" ||
          call->arg_size() == 0) {
        continue;
      }
      auto *formatArg = call->getArgOperand(0);
      if (llvm::isa<llvm::ConstantExpr>(formatArg)) {
        values.insert(renderNoelleLikeValue(formatArg));
      }
    }
  }
  for (auto const &boundaryValue : boundaryValues) {
    values.insert(boundaryValue);
  }
  return values;
}

static Values collectSCCsByKind(FunctionLoopAnalyses &analyses,
                                llvm::LoopInfo &LI,
                                std::function<bool(GenericSCC *)> predicate) {
  Values values;
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *attrs = content->getSCCAttrs();
  for (auto *scc : orderedSCCs(analyses, LI)) {
    auto *info = attrs->getSCCAttrs(scc);
    if (!predicate(info)) {
      continue;
    }
    std::vector<std::string> sccValues;
    for (auto &pair : scc->internalNodePairs()) {
      if (pair.first != nullptr) {
        sccValues.push_back(renderNoelleLikeValue(pair.first));
      }
    }
    values.insert(combineUnorderedValues(sccValues));
  }
  return values;
}

static Values collectLoopCarriedDependencies(FunctionLoopAnalyses &analyses,
                                             llvm::LoopInfo &LI) {
  Values values;
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *attrs = content->getSCCAttrs();
  for (auto *scc : attrs->getSCCsWithLoopCarriedDependencies()) {
    for (auto *dep : scc->getLoopCarriedDependences()) {
      values.insert(
          combineOrderedValues({valueToString(dep->getSrc()->getValue()),
                                valueToString(dep->getDst()->getValue())}));
    }
  }
  return values;
}

static Values collectStackClonableSCCsByAllocation(FunctionLoopAnalyses &analyses,
                                                   llvm::LoopInfo &LI) {
  Values values;
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *attrs = content->getSCCAttrs();
  std::map<llvm::AllocaInst *, std::set<std::string>> groupedValues;

  for (auto *info : attrs->getSCCsOfKind(GenericSCC::STACK_OBJECT_CLONABLE)) {
    auto *clonable =
        dynamic_cast<lotus::analysis::loop::StackObjectClonableSCC *>(info);
    if (clonable == nullptr || info->getSCC() == nullptr) {
      continue;
    }
    for (auto *allocation : clonable->getMemoryLocationsToClone()) {
      auto &group = groupedValues[allocation];
      for (auto &pair : info->getSCC()->internalNodePairs()) {
        if (pair.first != nullptr) {
          group.insert(renderNoelleLikeValue(pair.first));
        }
      }
    }
  }

  for (auto const &entry : groupedValues) {
    std::vector<std::string> sccValues(entry.second.begin(), entry.second.end());
    values.insert(combineUnorderedValues(sccValues));
  }
  return values;
}

static Values collectClonableSCCs(FunctionLoopAnalyses &analyses,
                                  llvm::LoopInfo &LI) {
  Values values;
  auto *content = analyses.getLoopContent(**LI.begin());
  auto *attrs = content->getSCCAttrs();
  auto *loopNode = content->getLoopHierarchyStructures();
  auto *topLoop = loopNode->getLoop();
  for (auto *scc : orderedAllSCCs(analyses, LI)) {
    auto *info = attrs->getSCCAttrs(scc);
    bool clonable = false;
    auto valuePairs = scc->internalNodePairs();
    bool hasLoopLocalComputation = false;
    bool onlyLoopLocalSimpleArithmetic = true;
    bool hasCalls = false;
    bool hasPointerArgument = false;
    bool singleAdministrative = false;

    bool onlyTerminators = true;
    for (auto &pair : valuePairs) {
      if (auto *arg = llvm::dyn_cast<llvm::Argument>(pair.first)) {
        hasPointerArgument |= arg->getType()->isPointerTy();
      }
      auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(pair.first);
      if (inst != nullptr && !llvm::isa<llvm::CmpInst>(inst) &&
          !inst->isTerminator()) {
        onlyTerminators = false;
      }
      if (inst == nullptr) {
        continue;
      }
      if (topLoop->isIncluded(inst)) {
        hasLoopLocalComputation = true;
        onlyLoopLocalSimpleArithmetic &=
            llvm::isa<llvm::BinaryOperator>(inst) && !inst->isTerminator() &&
            !llvm::isa<llvm::CmpInst>(inst);
      }
      hasCalls |= llvm::isa<llvm::CallBase>(inst);
    }
    if (valuePairs.size() == 1) {
      auto *inst =
          llvm::dyn_cast_or_null<llvm::Instruction>(valuePairs.front().first);
      singleAdministrative =
          inst != nullptr && (llvm::isa<llvm::PHINode>(inst) ||
                              llvm::isa<llvm::GetElementPtrInst>(inst) ||
                              llvm::isa<llvm::CastInst>(inst));
    }
    if (hasCalls || hasPointerArgument) {
      clonable = false;
    } else if (onlyTerminators) {
      clonable = true;
    } else if (info != nullptr &&
               info->getKind() == GenericSCC::LINEAR_INDUCTION_VARIABLE) {
      clonable = true;
    } else if (info != nullptr && !info->doesHaveMemoryDependencesWithin()) {
      if (info->getKind() == GenericSCC::LOOP_ITERATION) {
        clonable = !hasLoopLocalComputation || onlyLoopLocalSimpleArithmetic ||
                   singleAdministrative;
      } else if (auto *lc = dynamic_cast<LoopCarriedSCC *>(info)) {
        bool fullyContained = true;
        for (auto *dep : lc->getLoopCarriedDependences()) {
          auto *srcI = llvm::dyn_cast_or_null<llvm::Instruction>(
              dep->getSrc()->getValue());
          auto *dstI = llvm::dyn_cast_or_null<llvm::Instruction>(
              dep->getDst()->getValue());
          if (srcI == nullptr || dstI == nullptr) {
            fullyContained = false;
            break;
          }
          if (loopNode->getInnermostLoopThatContains(srcI) == topLoop ||
              loopNode->getInnermostLoopThatContains(dstI) == topLoop) {
            fullyContained = false;
            break;
          }
        }
        clonable = fullyContained;
      }
    }

    if (!clonable) {
      continue;
    }
    std::vector<std::string> sccValues;
    for (auto &pair : valuePairs) {
      if (pair.first != nullptr) {
        sccValues.push_back(renderNoelleLikeValue(pair.first));
      }
    }
    values.insert(combineUnorderedValues(sccValues));

    for (auto &pair : valuePairs) {
      auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(pair.first);
      if (call == nullptr) {
        continue;
      }
      auto *callee = call->getCalledFunction();
      if (callee == nullptr || callee->getName() != "printf" ||
          call->arg_size() == 0) {
        continue;
      }
      auto *formatArg = call->getArgOperand(0);
      if (llvm::isa<llvm::ConstantExpr>(formatArg)) {
        values.insert(renderNoelleLikeValue(formatArg));
      }
    }
  }
  return values;
}

static std::string sccSignature(LoopSCC *scc) {
  std::vector<std::string> sccValues;
  for (auto &pair : scc->internalNodePairs()) {
    if (pair.first != nullptr) {
      sccValues.push_back(renderNoelleLikeValue(pair.first));
    }
  }
  return combineUnorderedValues(sccValues);
}

static void expectSCCDAGStructuralInvariants(FunctionLoopAnalyses &analyses,
                                             llvm::LoopInfo &LI) {
  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(content->hasDependenceGraph());
  ASSERT_TRUE(content->hasSCCDAG());

  auto *ldg = content->getLoopDependenceGraph();
  auto *sccdag = content->getSCCDAG();
  ASSERT_NE(ldg, nullptr);
  ASSERT_NE(sccdag, nullptr);

  auto externalNodes = ldg->getExternalNodes();
  EXPECT_FALSE(externalNodes.empty())
      << "Parity fixtures should exercise boundary-node modeling";

  auto includedSCCs = sccdag->getSCCs();
  auto allSCCs = sccdag->getAllSCCs();
  ASSERT_FALSE(allSCCs.empty());
  EXPECT_GE(allSCCs.size(), includedSCCs.size());

  std::unordered_set<LoopSCC *> includedSet(includedSCCs.begin(),
                                            includedSCCs.end());
  bool sawExternalSCC = false;
  for (auto *scc : allSCCs) {
    ASSERT_NE(scc, nullptr);
    if (!scc->isIncludedInLoop()) {
      sawExternalSCC = true;
      EXPECT_EQ(includedSet.find(scc), includedSet.end());
      continue;
    }
    EXPECT_NE(includedSet.find(scc), includedSet.end());

    for (auto *succ : scc->getSuccessors()) {
      ASSERT_NE(succ, nullptr);
      auto succPreds = succ->getPredecessors();
      EXPECT_NE(std::find(succPreds.begin(), succPreds.end(), scc),
                succPreds.end());
      EXPECT_TRUE(sccdag->orderedBefore(scc, succ));
      EXPECT_FALSE(sccdag->orderedBefore(succ, scc));
    }
  }
  for (auto *first : allSCCs) {
    for (auto *second : allSCCs) {
      if (!sccdag->orderedBefore(first, second)) {
        continue;
      }
      for (auto *third : allSCCs) {
        if (sccdag->orderedBefore(second, third)) {
          EXPECT_TRUE(sccdag->orderedBefore(first, third));
        }
      }
    }
  }
  EXPECT_TRUE(sawExternalSCC)
      << "Parity fixtures should expose SCCDAG boundary nodes";
}

static std::string sortCompositeTokens(std::string value) {
  lotus::unittest::noelle_golden::Parser::trim(value);
  std::vector<std::string> tokens;
  std::string current;
  for (char ch : value) {
    if (ch == '|' || ch == ';') {
      lotus::unittest::noelle_golden::Parser::trim(current);
      if (!current.empty()) {
        tokens.push_back(current);
      }
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  lotus::unittest::noelle_golden::Parser::trim(current);
  if (!current.empty()) {
    tokens.push_back(current);
  }
  if (tokens.size() <= 1) {
    return tokens.empty() ? std::string{} : tokens.front();
  }
  std::sort(tokens.begin(), tokens.end());
  return combineOrderedValues(tokens);
}

static bool sectionMatchesAsUnorderedComposite(const GoldenFile &golden,
                                               const std::string &section,
                                               const Values &actual,
                                               std::string *diff = nullptr) {
  if (!golden.hasSection(section)) {
    if (diff != nullptr) {
      *diff = "missing golden section: " + section;
    }
    return false;
  }

  Values normalizedExpected;
  for (auto const &value : golden.getSection(section)) {
    normalizedExpected.insert(sortCompositeTokens(value));
  }

  Values normalizedActual;
  for (auto const &value : actual) {
    normalizedActual.insert(sortCompositeTokens(golden.normalizeValue(value)));
  }

  std::vector<std::string> missing;
  for (auto const &value : normalizedExpected) {
    if (normalizedActual.find(value) == normalizedActual.end()) {
      missing.push_back(value);
    }
  }
  std::vector<std::string> unexpected;
  for (auto const &value : normalizedActual) {
    if (normalizedExpected.find(value) == normalizedExpected.end()) {
      unexpected.push_back(value);
    }
  }

  if (missing.empty() && unexpected.empty()) {
    return true;
  }

  if (diff != nullptr) {
    std::string message = "section " + section;
    if (!missing.empty()) {
      message += " missing=" + std::to_string(missing.size()) + " [";
      for (size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) {
          message += " || ";
        }
        message += missing[i];
      }
      message += "]";
    }
    if (!unexpected.empty()) {
      message += (!missing.empty() ? " " : "") + std::string("unexpected=") +
                 std::to_string(unexpected.size()) + " [";
      for (size_t i = 0; i < unexpected.size(); ++i) {
        if (i != 0) {
          message += " || ";
        }
        message += unexpected[i];
      }
      message += "]";
    }
    *diff = std::move(message);
  }

  return false;
}

static void runCase(const std::string &name) {
  llvm::LLVMContext context;
  auto module = loadModule(llPath(name), context, "LoopParitySCCDAGAttrsTest");
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
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);
  analyses.materializeSCCAttrs(DT, PDT);

  expectSCCDAGStructuralInvariants(analyses, LI);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *attrs = content->getSCCAttrs();
  ASSERT_NE(attrs, nullptr);
  auto clonableByHeuristic = collectClonableSCCs(analyses, LI);
  for (auto *info : attrs->getSCCsOfKind(GenericSCC::STACK_OBJECT_CLONABLE)) {
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->getSCC(), nullptr);
    auto *clonable =
        dynamic_cast<lotus::analysis::loop::StackObjectClonableSCC *>(info);
    ASSERT_NE(clonable, nullptr);
    EXPECT_FALSE(clonable->getClonableMemoryObjects().empty())
        << "STACK_OBJECT_CLONABLE SCC must retain clonable-memory metadata, "
           "not just projected allocations";
    EXPECT_FALSE(clonable->getMemoryLocationsToClone().empty())
        << "STACK_OBJECT_CLONABLE SCC must identify at least one stack "
           "allocation to clone";
    EXPECT_FALSE(clonable->getLoopCarriedDependences().empty())
        << "STACK_OBJECT_CLONABLE SCC must still be justified by loop-carried "
           "dependences after refinement";
  }

  GoldenFile golden(goldenPath(name));
  std::string diff;
  if (golden.hasSection("scc with IV")) {
    EXPECT_TRUE(sectionMatches(
        golden, "scc with IV",
        collectSCCsByKind(analyses, LI,
                          [](GenericSCC *info) {
                            return info->getKind() ==
                                   GenericSCC::LINEAR_INDUCTION_VARIABLE;
                          }),
        &diff))
        << "Parity deviation for sccdag_attributes/" << name << ": "
        << diff;
  }
  if (golden.hasSection("reducible SCC")) {
    EXPECT_TRUE(
        sectionMatches(golden, "reducible SCC",
                       collectSCCsByKind(analyses, LI,
                                         [](GenericSCC *info) {
                                           return info->getKind() ==
                                                  GenericSCC::BINARY_REDUCTION;
                                         }),
                       &diff))
        << "Parity deviation for sccdag_attributes/" << name << ": "
        << diff;
  }
  (void)clonableByHeuristic;
}

TEST(LoopParitySCCDAGAttrsTest, SimpleMatchesNoelleGolden) {
  runCase("simple");
}
TEST(LoopParitySCCDAGAttrsTest, ReducibleAdditionMatchesNoelleGolden) {
  runCase("reducible_addition");
}
TEST(LoopParitySCCDAGAttrsTest, ClonableAllocaMatchesNoelleGolden) {
  runCase("clonable_alloca");
}

} // namespace
