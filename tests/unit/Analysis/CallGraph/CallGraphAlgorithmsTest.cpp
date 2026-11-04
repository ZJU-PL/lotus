#include "Analysis/CallGraph/AllocatedTypes.h"
#include "Analysis/CallGraph/CHA/CHAResolver.h"
#include "Analysis/CallGraph/CallGraph.h"
#include "Analysis/CallGraph/CallGraphAnalysisType.h"
#include "Analysis/CallGraph/CallGraphBuilder.h"
#include "Analysis/CallGraph/RTA/RTAResolver.h"
#include "Analysis/CallGraph/VTA/VTAResolver.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <set>
#include <string>

using namespace llvm;
using namespace lotus;

namespace {

using lotus::unittest::loadModule;

std::string getTestFilePath(const std::string &FileName) {
  return std::string(LOTUS_TYPE_HIERARCHY_LL_DIR) + "/" + FileName;
}

std::set<std::string> getCalleeNames(const CallGraph &CG, const Instruction *CS) {
  std::set<std::string> Names;
  for (const auto *F : CG.getCalleesOfCallAt(CS)) {
    Names.insert(F->getName().str());
  }
  return Names;
}

} // namespace

TEST(CallGraphAlgorithmsTest, CHAResolver_ResolvesAllSubtypes) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("call_graph_cha_rta_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);

  DIBasedTypeHierarchy TH(*M);
  LLVMVFTableProvider VTP(*M);
  CHAResolver CHA(M.get(), &VTP, &TH);

  auto CG = buildCallGraph(*M, CHA, {"main"});

  const auto *MainFn = M->getFunction("main");
  ASSERT_NE(nullptr, MainFn);

  const Instruction *VCall = nullptr;
  for (const auto &BB : *MainFn) {
    for (const auto &I : BB) {
      if (const auto *Call = dyn_cast<CallBase>(&I)) {
        if (isVirtualCall(Call, VTP)) {
          VCall = &I;
          break;
        }
      }
    }
  }

  ASSERT_NE(nullptr, VCall);
  auto Callees = getCalleeNames(CG, VCall);

  EXPECT_TRUE(Callees.count("_ZN4Base3fooEv"));
  EXPECT_TRUE(Callees.count("_ZN16DerivedAllocated3fooEv"));
  EXPECT_TRUE(Callees.count("_ZN18DerivedUnallocated3fooEv"));
  EXPECT_EQ(Callees.size(), 3U);
}

TEST(CallGraphAlgorithmsTest, RTAResolver_PrunesUnallocatedTypes) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("call_graph_cha_rta_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);

  DIBasedTypeHierarchy TH(*M);
  LLVMVFTableProvider VTP(*M);
  RTAResolver RTA(M.get(), &VTP, &TH);

  auto CG = buildCallGraph(*M, RTA, {"main"});

  const auto *MainFn = M->getFunction("main");
  ASSERT_NE(nullptr, MainFn);

  const Instruction *VCall = nullptr;
  for (const auto &BB : *MainFn) {
    for (const auto &I : BB) {
      if (const auto *Call = dyn_cast<CallBase>(&I)) {
        if (isVirtualCall(Call, VTP)) {
          VCall = &I;
          break;
        }
      }
    }
  }

  ASSERT_NE(nullptr, VCall);
  auto Callees = getCalleeNames(CG, VCall);

  EXPECT_TRUE(Callees.count("_ZN4Base3fooEv"));
  EXPECT_TRUE(Callees.count("_ZN16DerivedAllocated3fooEv"));
  EXPECT_FALSE(Callees.count("_ZN18DerivedUnallocated3fooEv"));
  EXPECT_EQ(Callees.size(), 2U);
}

TEST(CallGraphAlgorithmsTest, VTAResolver_PreciseVariableTypePropagation) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("call_graph_vta_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);

  LLVMVFTableProvider VTP(*M);
  DIBasedTypeHierarchy TH(*M);
  VTAResolver VTA(M.get(), &VTP, std::make_unique<RTAResolver>(M.get(), &VTP, &TH));

  auto CG = buildCallGraph(*M, VTA, {"main"});

  const auto *MainFn = M->getFunction("main");
  ASSERT_NE(nullptr, MainFn);

  std::vector<const Instruction *> VCalls;
  for (const auto &BB : *MainFn) {
    for (const auto &I : BB) {
      if (const auto *Call = dyn_cast<CallBase>(&I)) {
        if (isVirtualCall(Call, VTP)) {
          VCalls.push_back(&I);
        }
      }
    }
  }

  ASSERT_EQ(VCalls.size(), 2U);

  auto Callees1 = getCalleeNames(CG, VCalls[0]);
  auto Callees2 = getCalleeNames(CG, VCalls[1]);

  EXPECT_TRUE(Callees1.count("_ZN3Dog5speakEv"));
  EXPECT_FALSE(Callees1.count("_ZN3Cat5speakEv"));
  EXPECT_EQ(Callees1.size(), 1U);

  EXPECT_TRUE(Callees2.count("_ZN3Cat5speakEv"));
  EXPECT_FALSE(Callees2.count("_ZN3Dog5speakEv"));
  EXPECT_EQ(Callees2.size(), 1U);
}

TEST(CallGraphAlgorithmsTest, FunctionPointerResolution) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("call_graph_fnptr_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);

  LLVMVFTableProvider VTP(*M);
  DIBasedTypeHierarchy TH(*M);
  RTAResolver Res(M.get(), &VTP, &TH);

  auto CG = buildCallGraph(*M, Res, {"main"});

  const auto *ComputeFn = M->getFunction("_Z7computePFiiiEii");
  ASSERT_NE(nullptr, ComputeFn);

  const Instruction *IndirectCall = nullptr;
  for (const auto &BB : *ComputeFn) {
    for (const auto &I : BB) {
      if (const auto *Call = dyn_cast<CallBase>(&I)) {
        if (!Call->getCalledFunction() && !Call->isDebugOrPseudoInst()) {
          IndirectCall = &I;
          break;
        }
      }
    }
  }

  ASSERT_NE(nullptr, IndirectCall);
  auto Callees = getCalleeNames(CG, IndirectCall);

  EXPECT_TRUE(Callees.count("_Z3addii"));
  EXPECT_TRUE(Callees.count("_Z3subii"));
  EXPECT_EQ(Callees.size(), 2U);
}

TEST(CallGraphAlgorithmsTest, VTAResolver_WithPrecomputedBaseCG) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("call_graph_vta_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);

  LLVMVFTableProvider VTP(*M);
  DIBasedTypeHierarchy TH(*M);
  RTAResolver RTA(M.get(), &VTP, &TH);
  auto BaseCG = buildCallGraph(*M, RTA, {"main"});

  VTAResolver VTA(M.get(), &VTP, &BaseCG);
  auto CG = buildCallGraph(*M, VTA, {"main"});
  EXPECT_FALSE(CG.empty());
  EXPECT_GT(CG.getNumCallSites(), 0U);
}

TEST(CallGraphAlgorithmsTest, CallGraphBuilderConvenienceAPI) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("call_graph_cha_rta_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);

  auto CG_CHA = buildCallGraph(*M, CallGraphAnalysisType::CHA, {"main"});
  EXPECT_FALSE(CG_CHA.empty());
  EXPECT_GT(CG_CHA.getNumCallSites(), 0U);

  auto CG_RTA = buildCallGraph(*M, CallGraphAnalysisType::RTA, {"main"});
  EXPECT_FALSE(CG_RTA.empty());
  EXPECT_GT(CG_RTA.getNumCallSites(), 0U);

  auto CG_VTA = buildCallGraph(*M, CallGraphAnalysisType::VTA, {"main"});
  EXPECT_FALSE(CG_VTA.empty());
  EXPECT_GT(CG_VTA.getNumCallSites(), 0U);
}
