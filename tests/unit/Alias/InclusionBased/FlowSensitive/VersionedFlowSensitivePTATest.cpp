#include "Alias/InclusionBased/FlowSensitive/Versioned/VersionedFlowSensitivePTA.h"

#include "Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <unistd.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::alias;
using namespace lotus::unittest;
using PointsToSet = VersionedFlowSensitivePTA::PointsToSet;

class VersionedFlowSensitivePTATest : public LlvmModuleTest {};

TEST_F(VersionedFlowSensitivePTATest,
       ObjectsWithIdenticalFootprintsReuseVersionLabels) {
  constexpr uint32_t firstObject = 100;
  constexpr uint32_t secondObject = 101;
  SVFG graph;
  auto *store = new StoreSVFGNode(1, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {firstObject, secondObject});
  graph.addNode(store);
  SVFG::ObjectInfo info;
  info.isGlobal = true;
  info.isSingleton = true;
  graph.setObjectInfo(firstObject, info);
  graph.setObjectInfo(secondObject, info);

  VersionedFlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();
  EXPECT_EQ(solver.canonicalVersionObject(firstObject), firstObject);
  EXPECT_EQ(solver.canonicalVersionObject(secondObject), firstObject);
  EXPECT_EQ(stats.equivalentObjects, 1u);
  EXPECT_EQ(solver.getConsume(1, firstObject),
            solver.getConsume(1, secondObject));
}

TEST_F(VersionedFlowSensitivePTATest,
       VersioningThreadsProduceTheSameLabelsAsSequential) {
  auto module = parseModule(R"(
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    define i8* @main() {
    entry:
      %p = alloca i8*
      %q = alloca i8*
      store i8* @a, i8** %p
      store i8* @b, i8** %p
      store i8* @c, i8** %q
      %first = load i8*, i8** %p
      %second = load i8*, i8** %q
      ret i8* %first
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *second = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      if (candidate->getName() == "second")
        second = candidate;
  ASSERT_NE(second, nullptr);

  VersionedFlowSensitivePTA::Config sequentialConfig;
  sequentialConfig.versioningThreads = 1;
  VersionedFlowSensitivePTA sequential(*graph, sequentialConfig);
  sequential.solve();

  VersionedFlowSensitivePTA::Config parallelConfig;
  parallelConfig.versioningThreads = 4;
  VersionedFlowSensitivePTA parallel(*graph, parallelConfig);
  parallel.solve();

  EXPECT_EQ(sequential.pointsTo(second), parallel.pointsTo(second));
  EXPECT_EQ(sequential.statistics().versions, parallel.statistics().versions);
  EXPECT_EQ(sequential.statistics().equivalentObjects,
            parallel.statistics().equivalentObjects);

  SmallString<256> sequentialPath;
  SmallString<256> parallelPath;
  int sequentialFd = -1;
  int parallelFd = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-vfspta-seq", "txt",
                                            sequentialFd, sequentialPath));
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-vfspta-par", "txt",
                                            parallelFd, parallelPath));
  ::close(sequentialFd);
  ::close(parallelFd);
  ASSERT_TRUE(sequential.writeVersionedAnalysisResultToFile(
      sequentialPath.str().str()));
  ASSERT_TRUE(
      parallel.writeVersionedAnalysisResultToFile(parallelPath.str().str()));
  std::ifstream sequentialInput(sequentialPath.str().str());
  std::ifstream parallelInput(parallelPath.str().str());
  const std::string sequentialResult(
      (std::istreambuf_iterator<char>(sequentialInput)),
      std::istreambuf_iterator<char>());
  const std::string parallelResult(
      (std::istreambuf_iterator<char>(parallelInput)),
      std::istreambuf_iterator<char>());
  sys::fs::remove(sequentialPath);
  sys::fs::remove(parallelPath);
  EXPECT_EQ(sequentialResult, parallelResult);
}

TEST_F(VersionedFlowSensitivePTATest,
       IndirectFunctionEntryUsesDedicatedDeltaVersion) {
  auto module = parseModule(R"(
    define void @target(i8** %pointer) {
    entry:
      ret void
    }
    define i32 @main(void (i8**)* %function.pointer, i8** %pointer) {
    entry:
      call void %function.pointer(i8** %pointer)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  const Function *target = module->getFunction("target");
  const CallBase *call = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<CallBase>(&instruction))
      call = candidate;
  ASSERT_NE(target, nullptr);
  ASSERT_NE(call, nullptr);

  constexpr uint32_t object = 100;
  SVFG graph;
  auto *root = new DummySVFGNode(1, nullptr);
  auto *formalIn =
      new FormalInSVFGNode(2, nullptr, target, 1, PointsToSet{object}, 1);
  graph.addNode(root);
  graph.addNode(formalIn);
  graph.setObjectDebug(object, "memory");
  graph.addCalleeToIndCallSite(target, call);

  VersionedFlowSensitivePTA solver(graph);
  solver.solve();
  const auto rootVersion = solver.getConsume(root->getId(), object);
  const auto deltaVersion = solver.getConsume(formalIn->getId(), object);
  EXPECT_NE(rootVersion, VersionedFlowSensitivePTA::InvalidVersion);
  EXPECT_NE(deltaVersion, VersionedFlowSensitivePTA::InvalidVersion);
  EXPECT_NE(rootVersion, deltaVersion);
  EXPECT_EQ(deltaVersion, solver.getYield(formalIn->getId(), object));
}

TEST_F(VersionedFlowSensitivePTATest,
       NewlyDiscoveredDeltaNodeIsWiredWithoutRelabeling) {
  auto module = parseModule(R"(
    define void @target(i8** %pointer) {
    entry:
      ret void
    }
    define i32 @main(void (i8**)* %function.pointer, i8** %pointer) {
    entry:
      call void %function.pointer(i8** %pointer)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  const Function *target = module->getFunction("target");
  const CallBase *call = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<CallBase>(&instruction))
      call = candidate;
  ASSERT_NE(target, nullptr);
  ASSERT_NE(call, nullptr);

  constexpr uint32_t functionObject = 100;
  constexpr uint32_t memoryObject = 200;
  constexpr uint32_t functionPointerId = 42;
  SVFG graph;
  auto *address = new AddrSVFGNode(1, nullptr, target, functionObject);
  address->setValueId(functionPointerId);
  auto *source = new DummySVFGNode(2, nullptr);
  auto *formalIn =
      new FormalInSVFGNode(3, nullptr, target, 1, PointsToSet{memoryObject}, 1);
  graph.addNode(address);
  graph.addNode(source);
  graph.addNode(formalIn);
  graph.addFormalIn(target, formalIn);
  graph.setObjectValue(functionObject, target);
  SVFG::ObjectInfo functionInfo;
  functionInfo.isFunction = true;
  functionInfo.isConstant = true;
  graph.setObjectInfo(functionObject, functionInfo);
  graph.setObjectDebug(functionObject, "target");
  graph.setObjectDebug(memoryObject, "memory");
  graph.addIndCallSite(functionPointerId, call);
  // Production graphs carry this index from the pre-analysis; it is what makes
  // the function entry a delta node.
  graph.addCalleeToIndCallSite(target, call);

  bool connected = false;
  VersionedFlowSensitivePTA::Config config;
  config.connectIndirectCall = [&](const CallBase *callSite,
                                   const Function *callee) {
    if (connected || callSite != call || callee != target)
      return false;
    connected = true;
    graph.addCalleeToIndCallSite(target, call);
    graph.addEdge(source, formalIn, SVFGEdgeK::IntraIndirect, call,
                  PointsToSet{memoryObject});
    return true;
  };

  VersionedFlowSensitivePTA solver(graph, std::move(config));
  const auto &stats = solver.solve();
  EXPECT_TRUE(connected);
  // The delta architecture prelabels delta nodes with a dedicated version, so
  // materialising a new edge only wires a reliance: no relabelling happens
  // beyond the initial one.
  EXPECT_EQ(stats.relabelings, 1u);
  EXPECT_EQ(stats.deltaRelabelings, 0u);
  EXPECT_GT(stats.deltaVersionUpdates, 0u);
  EXPECT_NE(solver.getConsume(source->getId(), memoryObject),
            solver.getConsume(formalIn->getId(), memoryObject));
}

TEST_F(VersionedFlowSensitivePTATest,
       StorePrelabelsProduceDistinctConsumeAndYieldVersions) {
  auto module = parseModule(R"(
    @slot = global i8* null
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      store i8* @x, i8** @slot
      store i8* @y, i8** @slot
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  auto instruction = module->getFunction("main")->getEntryBlock().begin();
  auto *storeXInstruction = cast<StoreInst>(&*instruction++);
  auto *storeYInstruction = cast<StoreInst>(&*instruction++);
  auto *loadInstruction = cast<LoadInst>(&*instruction);

  constexpr uint32_t xObject = 100;
  constexpr uint32_t yObject = 101;
  constexpr uint32_t slotObject = 200;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *storeX = new StoreSVFGNode(3, nullptr, storeXInstruction, 0);
  auto *storeY = new StoreSVFGNode(4, nullptr, storeYInstruction, 0);
  auto *load = new LoadSVFGNode(5, nullptr, loadInstruction, 0);
  storeX->setMemoryDef(1, 1, {slotObject});
  storeY->setMemoryDef(1, 2, {slotObject});
  load->setMemoryUse(1, 2, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, storeX, storeY, load})
    graph.addNode(node);
  graph.setValueNode(module->getGlobalVariable("x"), x->getId());
  graph.setValueNode(module->getGlobalVariable("y"), y->getId());
  graph.setValueNode(loadInstruction, load->getId());
  SVFG::ObjectInfo singleton;
  singleton.isGlobal = true;
  singleton.isSingleton = true;
  graph.setObjectInfo(xObject, singleton);
  graph.setObjectInfo(yObject, singleton);
  graph.setObjectInfo(slotObject, singleton);
  graph.setObjectDebug(slotObject, "slot");
  graph.addEdge(x, storeX, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, storeY, SVFGEdgeK::IntraDirect);
  graph.addEdge(storeX, storeY, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});
  graph.addEdge(storeY, load, SVFGEdgeK::IntraIndirect, nullptr, {slotObject});

  VersionedFlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();

  const auto storeXConsume = solver.getConsume(storeX->getId(), slotObject);
  const auto storeXYield = solver.getYield(storeX->getId(), slotObject);
  const auto storeYConsume = solver.getConsume(storeY->getId(), slotObject);
  const auto storeYYield = solver.getYield(storeY->getId(), slotObject);
  EXPECT_NE(storeXConsume, VersionedFlowSensitivePTA::InvalidVersion);
  EXPECT_NE(storeXConsume, storeXYield);
  EXPECT_EQ(storeXYield, storeYConsume);
  EXPECT_NE(storeYConsume, storeYYield);
  EXPECT_EQ(storeYYield, solver.getConsume(load->getId(), slotObject));
  EXPECT_EQ(solver.versionedPointsTo(slotObject, storeYYield),
            PointsToSet({yObject}));
  EXPECT_EQ(solver.pointsTo(load), PointsToSet({yObject}));
  const auto &dependentStatements =
      solver.getDependentStatements(slotObject, storeXConsume);
  EXPECT_NE(std::find(dependentStatements.begin(), dependentStatements.end(),
                      storeX->getId()),
            dependentStatements.end());
  EXPECT_EQ(stats.strongUpdates, 2u);
  EXPECT_GT(stats.statementReliances, 0u);
  EXPECT_GT(stats.versions, 1u);
}

TEST_F(VersionedFlowSensitivePTATest,
       StrongUpdateDoesNotMeldKilledGlobalInitializer) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @slot = global i8* @x
    define i8* @main() {
    entry:
      store i8* @y, i8** @slot
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  const StoreInst *store = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
    if (const auto *candidate = dyn_cast<StoreInst>(&instruction))
      store = candidate;
  }
  ASSERT_NE(load, nullptr);
  ASSERT_NE(store, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    hasX |= graph->getObjectValue(object) == module->getGlobalVariable("x");
    hasY |= graph->getObjectValue(object) == module->getGlobalVariable("y");
  }
  EXPECT_FALSE(hasX);
  EXPECT_TRUE(hasY);

  const auto *storeNode = dyn_cast_or_null<StoreSVFGNode>(graph->getDef(store));
  ASSERT_NE(storeNode, nullptr);
  ASSERT_EQ(storeNode->getMemoryPointsTo().size(), 1u);
  const uint32_t slotObject = *storeNode->getMemoryPointsTo().begin();
  const auto consumed = solver.getConsume(storeNode->getId(), slotObject);
  const auto yielded = solver.getYield(storeNode->getId(), slotObject);
  const auto &reliant = solver.getReliantVersions(slotObject, consumed);
  EXPECT_EQ(std::find(reliant.begin(), reliant.end(), yielded), reliant.end());

  SmallString<256> path;
  int fileDescriptor = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-vfspta", "txt",
                                            fileDescriptor, path));
  ::close(fileDescriptor);
  ASSERT_TRUE(solver.writeVersionedAnalysisResultToFile(path.str().str()));

  VersionedFlowSensitivePTA restored(*graph);
  ASSERT_TRUE(restored.readVersionedAnalysisResultFromFile(path.str().str()));
  sys::fs::remove(path);
  EXPECT_EQ(restored.pointsTo(load), *result);
  EXPECT_EQ(restored.statistics().versionedFacts,
            solver.statistics().versionedFacts);
}

TEST_F(VersionedFlowSensitivePTATest,
       NonRecursiveCalleeStackObjectUsesStrongUpdates) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define i8* @helper() {
    entry:
      %slot = alloca i8*
      store i8* @x, i8** %slot
      store i8* @y, i8** %slot
      %result = load i8*, i8** %slot
      ret i8* %result
    }
    define i8* @main() {
    entry:
      %result = call i8* @helper()
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("helper")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  const auto &stats = solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->getObjectValue(object) == module->getGlobalVariable("y");
  }));
  EXPECT_EQ(stats.strongUpdates, 2u);
  EXPECT_EQ(stats.weakUpdates, 0u);
}

TEST_F(VersionedFlowSensitivePTATest,
       RecursiveCalleeStackObjectUsesWeakUpdates) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define i8* @recursive(i1 %stop) {
    entry:
      %slot = alloca i8*
      store i8* @x, i8** %slot
      br i1 %stop, label %exit, label %recurse
    recurse:
      %ignored = call i8* @recursive(i1 true)
      store i8* @y, i8** %slot
      br label %exit
    exit:
      %result = load i8*, i8** %slot
      ret i8* %result
    }
    define i8* @main() {
    entry:
      %result = call i8* @recursive(i1 false)
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  const auto &stats = solver.solve();
  EXPECT_EQ(stats.strongUpdates, 0u);
  EXPECT_GE(stats.weakUpdates, 2u);
}

TEST_F(VersionedFlowSensitivePTATest,
       MeldJoinCombinesBypassAndStrongUpdateVersions) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @slot = global i8* @x
    define i8* @main(i1 %condition) {
    entry:
      br i1 %condition, label %update, label %join
    update:
      store i8* @y, i8** @slot
      br label %join
    join:
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    hasX |= graph->getObjectValue(object) == module->getGlobalVariable("x");
    hasY |= graph->getObjectValue(object) == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(hasX);
  EXPECT_TRUE(hasY);
}

TEST_F(VersionedFlowSensitivePTATest,
       MeldJoinWithTwoDefinitionsDoesNotRestoreInitializer) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @z = global i8 0
    @slot = global i8* @x
    define i8* @main(i1 %condition) {
    entry:
      br i1 %condition, label %left, label %right
    left:
      store i8* @y, i8** @slot
      br label %join
    right:
      store i8* @z, i8** @slot
      br label %join
    join:
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  auto pointsToGlobal = [&](StringRef name) {
    return std::any_of(result->begin(), result->end(), [&](uint32_t object) {
      return graph->getObjectValue(object) == module->getGlobalVariable(name);
    });
  };
  EXPECT_FALSE(pointsToGlobal("x"));
  EXPECT_TRUE(pointsToGlobal("y"));
  EXPECT_TRUE(pointsToGlobal("z"));
}

TEST_F(VersionedFlowSensitivePTATest,
       IndirectCallTopologyChangeWiresDeltaVersionsIncrementally) {
  auto module = parseModule(R"(
    @y = global i8 0
    @function.pointer = global void (i8**)* @target
    define void @target(i8** %pointer) {
    entry:
      store i8* @y, i8** %pointer
      ret void
    }
    define i8* @main() {
    entry:
      %slot = alloca i8*
      %callee = load void (i8**)*, void (i8**)** @function.pointer
      call void %callee(i8** %slot)
      %result = load i8*, i8** %slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const Function *mainFunction = module->getFunction("main");
  const Function *targetFunction = module->getFunction("target");
  ASSERT_NE(mainFunction, nullptr);
  ASSERT_NE(targetFunction, nullptr);
  const CallBase *indirectCall = nullptr;
  for (const Instruction &instruction : instructions(mainFunction)) {
    const auto *call = dyn_cast<CallBase>(&instruction);
    if (call && !call->getCalledFunction())
      indirectCall = call;
  }
  ASSERT_NE(indirectCall, nullptr);
  const SVFGNode *functionPointerNode =
      graph->getValueNode(indirectCall->getCalledOperand());
  ASSERT_NE(functionPointerNode, nullptr);
  const uint32_t functionPointerId = functionPointerNode->hasValueId()
                                         ? functionPointerNode->getValueId()
                                         : functionPointerNode->getId();
  EXPECT_FALSE(graph->getIndCallSites(functionPointerId).empty());
  EXPECT_TRUE(graph->getConnectedCallees(indirectCall).empty());

  VersionedFlowSensitivePTA::Config solverConfig;
  std::size_t connectorCalls = 0;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    ++connectorCalls;
    if (callSite != indirectCall || target != targetFunction)
      return false;
    std::vector<SVFGEdge *> edges;
    return builder.connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                   target, edges);
  };
  VersionedFlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();
  const auto callees = solver.pointsTo(indirectCall->getCalledOperand());
  ASSERT_TRUE(callees.has_value());
  EXPECT_TRUE(
      std::any_of(callees->begin(), callees->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == targetFunction;
      }));
  EXPECT_GT(stats.indirectCallEdges, 0u);
  EXPECT_EQ(stats.relabelings, 1u);
  EXPECT_EQ(stats.deltaRelabelings, 0u);
  EXPECT_GT(stats.deltaVersionUpdates, 0u);
  EXPECT_GT(connectorCalls, 0u);

  // The store in the newly connected callee must reach the load after the
  // call purely through the delta reliance wiring (no relabelling).
  const LoadInst *result = nullptr;
  for (const Instruction &instruction : instructions(mainFunction))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      if (candidate->getName() == "result")
        result = candidate;
  ASSERT_NE(result, nullptr);
  const auto pointsTo = solver.pointsTo(result);
  ASSERT_TRUE(pointsTo.has_value());
  ASSERT_EQ(pointsTo->size(), 1u);
  EXPECT_EQ(graph->getObjectValue(*pointsTo->begin()),
            module->getGlobalVariable("y"));
}

TEST_F(VersionedFlowSensitivePTATest,
       VoidIndirectCallWithoutValueEdgesStillUpdatesCallGraph) {
  auto module = parseModule(R"(
    @function.pointer = global void ()* @target
    define void @target() {
    entry:
      ret void
    }
    define i32 @main() {
    entry:
      %callee = load void ()*, void ()** @function.pointer
      call void %callee()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const Function *mainFunction = module->getFunction("main");
  const Function *targetFunction = module->getFunction("target");
  const CallBase *indirectCall = nullptr;
  for (const Instruction &instruction : instructions(mainFunction)) {
    const auto *call = dyn_cast<CallBase>(&instruction);
    if (call && !call->getCalledFunction())
      indirectCall = call;
  }
  ASSERT_NE(indirectCall, nullptr);

  VersionedFlowSensitivePTA::Config solverConfig;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    std::vector<SVFGEdge *> edges;
    return builder.connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                   target, edges);
  };
  VersionedFlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();
  EXPECT_NE(graph->getConnectedCallees(indirectCall).count(targetFunction), 0u);
  EXPECT_GT(stats.indirectCallEdges, 0u);
}

TEST_F(VersionedFlowSensitivePTATest,
       MemcpyProducesNewDestinationVersionAndCopiesPointer) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* @y, i8** %source.field
      store i8* @x, i8** %destination.field
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 8, i1 false)
      %result = load i8*, i8** %destination.field
      ret i8* %result
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    hasX |= graph->getObjectValue(object) == module->getGlobalVariable("x");
    hasY |= graph->getObjectValue(object) == module->getGlobalVariable("y");
  }
  EXPECT_FALSE(hasX);
  EXPECT_TRUE(hasY);
}

TEST_F(VersionedFlowSensitivePTATest,
       PartialMemcpyPreservesVersionOutsideCopiedRange) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    @z = global i8 0
    @w = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.0 = getelementptr %S, %S* %source, i32 0, i32 0
      %source.1 = getelementptr %S, %S* %source, i32 0, i32 1
      %destination.0 = getelementptr %S, %S* %destination, i32 0, i32 0
      %destination.1 = getelementptr %S, %S* %destination, i32 0, i32 1
      store i8* @x, i8** %source.0
      store i8* @y, i8** %source.1
      store i8* @z, i8** %destination.0
      store i8* @w, i8** %destination.1
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 8, i1 false)
      %copied = load i8*, i8** %destination.0
      %preserved = load i8*, i8** %destination.1
      ret i8* %copied
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *copied = nullptr;
  const LoadInst *preserved = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    const auto *load = dyn_cast<LoadInst>(&instruction);
    if (load && load->getName() == "copied")
      copied = load;
    else if (load && load->getName() == "preserved")
      preserved = load;
  }
  ASSERT_NE(copied, nullptr);
  ASSERT_NE(preserved, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  auto pointsToGlobal = [&](const LoadInst *load, StringRef name) {
    const auto result = solver.pointsTo(load);
    return result &&
           std::any_of(result->begin(), result->end(), [&](uint32_t object) {
             return graph->getObjectValue(object) ==
                    module->getGlobalVariable(name);
           });
  };
  EXPECT_TRUE(pointsToGlobal(copied, "x"));
  EXPECT_FALSE(pointsToGlobal(copied, "z"));
  EXPECT_TRUE(pointsToGlobal(preserved, "w"));
  EXPECT_FALSE(pointsToGlobal(preserved, "y"));
}

TEST_F(VersionedFlowSensitivePTATest, ZeroMemsetProducesEmptyPointerVersion) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %destination = alloca %S
      %field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* @x, i8** %field
      %bytes = bitcast %S* %destination to i8*
      call void @llvm.memset.p0i8.i64(i8* %bytes, i8 0,
                                      i64 8, i1 false)
      %result = load i8*, i8** %field
      ret i8* %result
    }
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST_F(VersionedFlowSensitivePTATest, ZeroLengthMemsetPreservesPointerVersion) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %destination = alloca %S
      %field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* @x, i8** %field
      %bytes = bitcast %S* %destination to i8*
      call void @llvm.memset.p0i8.i64(i8* %bytes, i8 0,
                                      i64 0, i1 false)
      %result = load i8*, i8** %field
      ret i8* %result
    }
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->getObjectValue(object) == module->getGlobalVariable("x");
  }));
}

TEST_F(VersionedFlowSensitivePTATest,
       StoreWithUnknownLhsDoesNotWriteNonTargetVersions) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @other = global i8 0
    define i8* @main() {
    entry:
      %cell = alloca i8*
      %alt = alloca i8*
      %pp = alloca i8**
      %p = alloca i8**
      store i8* @y, i8** %cell
      store i8* @other, i8** %alt
      store i8** %alt, i8*** %pp
      store i8** %cell, i8*** %pp
      %q = load i8**, i8*** %pp
      store i8** %q, i8*** %p
      %slot = load i8**, i8*** %p
      store i8* @x, i8** %slot
      %result = load i8*, i8** %alt
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  config.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *result = nullptr;
  const StoreInst *indirectStore = nullptr;
  const AllocaInst *altAlloca = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      result = candidate;
    if (const auto *candidate = dyn_cast<StoreInst>(&instruction))
      if (candidate->getValueOperand() == module->getGlobalVariable("x"))
        indirectStore = candidate;
    if (const auto *candidate = dyn_cast<AllocaInst>(&instruction))
      if (candidate->getName() == "alt")
        altAlloca = candidate;
  }
  ASSERT_NE(result, nullptr);
  ASSERT_NE(indirectStore, nullptr);
  ASSERT_NE(altAlloca, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();

  // The store through %slot only targets %cell even though the pre-analysis
  // targets include %alt.  The load of %alt must therefore still see @other.
  const auto pointsTo = solver.pointsTo(result);
  ASSERT_TRUE(pointsTo.has_value());
  ASSERT_EQ(pointsTo->size(), 1u);
  EXPECT_EQ(graph->getObjectValue(*pointsTo->begin()),
            module->getGlobalVariable("other"));
}


TEST_F(VersionedFlowSensitivePTATest,
       DeadStoreThroughBottomPointerDoesNotClobberMemory) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define i8* @main(i1 %condition) {
    entry:
      %p = alloca i8**
      %cell = alloca i8*
      store i8* @y, i8** %cell
      br i1 %condition, label %init, label %dead
    init:
      store i8** %cell, i8*** %p
      br label %end
    dead:
      %q = load i8**, i8*** %p
      store i8* @x, i8** %q
      br label %end
    end:
      %result = load i8*, i8** %cell
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  config.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *result = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      result = candidate;
  ASSERT_NE(result, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto pointsTo = solver.pointsTo(result);
  ASSERT_TRUE(pointsTo.has_value());
  ASSERT_EQ(pointsTo->size(), 1u);
  EXPECT_EQ(graph->getObjectValue(*pointsTo->begin()),
            module->getGlobalVariable("y"));
}

TEST_F(VersionedFlowSensitivePTATest,
       DeltaSourceEdgesFromFormalOutNeedNoRelabeling) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @function.pointer = global i8** (i8**)* @target
    define i8** @target(i8** %pointer) {
    entry:
      store i8* @y, i8** %pointer
      ret i8** %pointer
    }
    define i8* @main() {
    entry:
      %slot = alloca i8*
      store i8* @x, i8** %slot
      %callee = load i8** (i8**)*, i8** (i8**)** @function.pointer
      %returned = call i8** %callee(i8** %slot)
      %result = load i8*, i8** %returned
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const Function *mainFunction = module->getFunction("main");
  const Function *targetFunction = module->getFunction("target");
  ASSERT_NE(mainFunction, nullptr);
  ASSERT_NE(targetFunction, nullptr);
  const CallBase *indirectCall = nullptr;
  for (const Instruction &instruction : instructions(mainFunction)) {
    const auto *call = dyn_cast<CallBase>(&instruction);
    if (call && !call->getCalledFunction())
      indirectCall = call;
  }
  ASSERT_NE(indirectCall, nullptr);
  EXPECT_TRUE(graph->getConnectedCallees(indirectCall).empty());

  VersionedFlowSensitivePTA::Config solverConfig;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    if (callSite != indirectCall || target != targetFunction)
      return false;
    std::vector<SVFGEdge *> edges;
    return builder.connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                   target, edges);
  };
  VersionedFlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();

  // The caller's slot is written in the callee and read after the call; the
  // fact has to travel over formal-out -> actual-out delta edges.
  const LoadInst *result = nullptr;
  for (const Instruction &instruction : instructions(mainFunction))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      if (candidate->getName() == "result")
        result = candidate;
  ASSERT_NE(result, nullptr);
  const auto pointsTo = solver.pointsTo(result);
  ASSERT_TRUE(pointsTo.has_value());
  ASSERT_EQ(pointsTo->size(), 1u);
  EXPECT_EQ(graph->getObjectValue(*pointsTo->begin()),
            module->getGlobalVariable("y"));

  EXPECT_EQ(stats.relabelings, 1u);
  EXPECT_EQ(stats.deltaRelabelings, 0u);
  EXPECT_GT(stats.deltaSourceNodes, 0u);
  EXPECT_GT(stats.deltaVersionUpdates, 0u);
}

TEST_F(VersionedFlowSensitivePTATest,
       ArrayGlobalInitializerMatchesSparseFlowSensitive) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @array = global [2 x i8*] [i8* @x, i8* @y]
    define i8* @main() {
    entry:
      %element = getelementptr inbounds [2 x i8*], [2 x i8*]* @array, i64 0, i64 1
      %result = load i8*, i8** %element
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA sparse(*graph);
  sparse.solve();
  VersionedFlowSensitivePTA versioned(*graph);
  versioned.solve();

  const auto sparseResult = sparse.pointsTo(load);
  const auto versionedResult = versioned.pointsTo(load);
  ASSERT_TRUE(sparseResult.has_value());
  ASSERT_TRUE(versionedResult.has_value());
  EXPECT_EQ(*sparseResult, *versionedResult);

  auto pointsToY = [&](const PointsToSet &set) {
    return std::any_of(set.begin(), set.end(), [&](uint32_t object) {
      return graph->getObjectValue(object) == module->getGlobalVariable("y");
    });
  };
  EXPECT_TRUE(pointsToY(*sparseResult));
  EXPECT_TRUE(pointsToY(*versionedResult));
}

TEST_F(VersionedFlowSensitivePTATest,
       RestoredSolutionReconnectsIndirectCallEdges) {
  auto module = parseModule(R"(
    @y = global i8 0
    @function.pointer = global void (i8**)* @target
    define void @target(i8** %pointer) {
    entry:
      store i8* @y, i8** %pointer
      ret void
    }
    define i8* @main() {
    entry:
      %slot = alloca i8*
      %callee = load void (i8**)*, void (i8**)** @function.pointer
      call void %callee(i8** %slot)
      %result = load i8*, i8** %slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  const Function *mainFunction = module->getFunction("main");
  ASSERT_NE(mainFunction, nullptr);
  const CallBase *indirectCall = nullptr;
  const LoadInst *result = nullptr;
  for (const Instruction &instruction : instructions(mainFunction)) {
    if (const auto *call = dyn_cast<CallBase>(&instruction))
      if (!call->getCalledFunction())
        indirectCall = call;
    if (const auto *load = dyn_cast<LoadInst>(&instruction))
      if (load->getName() == "result")
        result = load;
  }
  ASSERT_NE(indirectCall, nullptr);
  ASSERT_NE(result, nullptr);

  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  auto connector = [](SVFGBuilder &source, SVFG *targetGraph) {
    return [&source, targetGraph](const CallBase *callSite,
                                  const Function *callee) {
      std::vector<SVFGEdge *> edges;
      return source.connectCallSiteToCalleeOnTheFly(targetGraph, callSite,
                                                    callee, edges);
    };
  };

  SmallString<256> path;
  int fileDescriptor = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-vfspta-resume", "txt",
                                            fileDescriptor, path));
  ::close(fileDescriptor);

  VersionedFlowSensitivePTA::Config solvedConfig;
  solvedConfig.connectIndirectCall = connector(builder, graph.get());
  VersionedFlowSensitivePTA solved(*graph, std::move(solvedConfig));
  solved.solve();
  const auto solvedPointsTo = solved.pointsTo(result);
  ASSERT_TRUE(solvedPointsTo.has_value());
  ASSERT_FALSE(graph->getConnectedCallees(indirectCall).empty());
  ASSERT_TRUE(std::any_of(
      solvedPointsTo->begin(), solvedPointsTo->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == module->getGlobalVariable("y");
      }));
  ASSERT_TRUE(solved.writeVersionedAnalysisResultToFile(path.str().str()));

  // Resume into a fresh graph built without any indirect-call edges.
  ICFG resumeIcfg;
  ICFGBuilder resumeIcfgBuilder(&resumeIcfg);
  resumeIcfgBuilder.build(module.get());
  SVFGBuilder resumeBuilder(graphConfig);
  std::unique_ptr<SVFG> resumeGraph(resumeBuilder.build(&resumeIcfg));
  ASSERT_NE(resumeGraph, nullptr);
  ASSERT_TRUE(resumeGraph->getConnectedCallees(indirectCall).empty());
  ASSERT_NE(resumeGraph->getValueNode(result), nullptr);
  ASSERT_EQ(graph->getValueNode(result)->getId(),
            resumeGraph->getValueNode(result)->getId());

  VersionedFlowSensitivePTA::Config resumeConfig;
  resumeConfig.connectIndirectCall =
      connector(resumeBuilder, resumeGraph.get());
  VersionedFlowSensitivePTA restored(*resumeGraph, std::move(resumeConfig));
  ASSERT_TRUE(restored.readVersionedAnalysisResultFromFile(path.str().str()));
  sys::fs::remove(path);

  // Reading must replay the on-the-fly connections the saved solution needs.
  EXPECT_FALSE(resumeGraph->getConnectedCallees(indirectCall).empty());

  auto objectValues = [](SVFG &source, const PointsToSet &set) {
    std::set<const Value *> values;
    for (uint32_t object : set)
      values.insert(source.getObjectValue(object));
    return values;
  };
  const auto restoredPointsTo = restored.pointsTo(result);
  ASSERT_TRUE(restoredPointsTo.has_value());
  EXPECT_EQ(objectValues(*graph, *solvedPointsTo),
            objectValues(*resumeGraph, *restoredPointsTo));
}

TEST_F(VersionedFlowSensitivePTATest, TimeLimitReportsTimeout) {
  SVFG graph;
  VersionedFlowSensitivePTA::Config config;
  config.timeLimitSeconds = -1; // already expired
  VersionedFlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_TRUE(solver.timedOut());
  EXPECT_TRUE(solver.statistics().timedOut);
  const std::string report = solver.statisticsReport();
  EXPECT_NE(report.find("timed out: yes"), std::string::npos);
  EXPECT_NE(report.find("svfgHasSU: no"), std::string::npos);
}

TEST_F(VersionedFlowSensitivePTATest,
       StatisticsReportDetectsStrongUpdateSites) {
  auto module = parseModule(R"(
    define void @f() {
    entry:
      %p = alloca i32
      store i32 1, i32* %p
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  EXPECT_TRUE(solver.statistics().svfgHasSU);
  const std::string report = solver.statisticsReport();
  EXPECT_NE(report.find("svfgHasSU: yes"), std::string::npos);
}

TEST_F(VersionedFlowSensitivePTATest,
       RecursiveTopologyChangeWithoutStrongUpdatesAvoidsRelabeling) {
  auto module = parseModule(R"(
    @function.pointer = global void ()* @target
    define void @target() {
    entry:
      %callee = load void ()*, void ()** @function.pointer
      call void %callee()
      ret void
    }
    define i32 @main() {
    entry:
      %callee = load void ()*, void ()** @function.pointer
      call void %callee()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  VersionedFlowSensitivePTA::Config solverConfig;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    std::vector<SVFGEdge *> edges;
    return builder.connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                   target, edges);
  };
  VersionedFlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();
  EXPECT_GT(stats.indirectCallEdges, 0u);
  // target becomes self-recursive, but it contains no strong-update site, so
  // the existing version labels stay valid: only the initial labelling runs.
  EXPECT_EQ(stats.relabelings, 1u);
  EXPECT_GT(stats.deltaVersionUpdates, 0u);
}

TEST_F(VersionedFlowSensitivePTATest,
       RecursiveTopologyChangeRelabelsWhenStrongUpdateMayChange) {
  auto module = parseModule(R"(
    @function.pointer = global void ()* @target
    define void @target() {
    entry:
      %slot = alloca i32
      store i32 1, i32* %slot
      %callee = load void ()*, void ()** @function.pointer
      call void %callee()
      ret void
    }
    define i32 @main() {
    entry:
      %callee = load void ()*, void ()** @function.pointer
      call void %callee()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  VersionedFlowSensitivePTA::Config solverConfig;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    std::vector<SVFGEdge *> edges;
    return builder.connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                   target, edges);
  };
  VersionedFlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();
  EXPECT_GT(stats.indirectCallEdges, 0u);
  // The store in the newly recursive function must switch from strong to weak,
  // which invalidates the labels: one relabel per solve epoch is expected.
  EXPECT_EQ(stats.relabelings, 2u);
  EXPECT_EQ(stats.strongUpdates, 0u);
}

TEST_F(VersionedFlowSensitivePTATest, ReadsLegacyV1ResultFile) {
  auto module = parseModule(R"(
    @y = global i8 0
    define i8* @main() {
    entry:
      %slot = alloca i8*
      store i8* @y, i8** %slot
      %result = load i8*, i8** %slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solved(*graph);
  solved.solve();
  const auto pointsTo = solved.pointsTo(load);
  ASSERT_TRUE(pointsTo.has_value());

  SmallString<256> currentPath;
  int currentFD = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-vfspta-v2", "txt", currentFD,
                                            currentPath));
  ::close(currentFD);
  ASSERT_TRUE(
      solved.writeVersionedAnalysisResultToFile(currentPath.str().str()));

  std::string content;
  {
    std::ifstream input(currentPath.str().str());
    content.assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
  }
  const std::string header = "LOTUS_VFSPTA 2";
  const auto headerPos = content.find(header);
  ASSERT_NE(headerPos, std::string::npos);
  content.replace(headerPos, header.size(), "LOTUS_VFSPTA 1");

  SmallString<256> legacyPath;
  int legacyFD = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-vfspta-v1", "txt", legacyFD,
                                            legacyPath));
  ::close(legacyFD);
  {
    std::ofstream output(legacyPath.str().str(), std::ios::trunc);
    output << content;
  }

  VersionedFlowSensitivePTA restored(*graph);
  EXPECT_TRUE(
      restored.readVersionedAnalysisResultFromFile(legacyPath.str().str()));
  EXPECT_EQ(restored.pointsTo(load), *pointsTo);
  sys::fs::remove(currentPath);
  sys::fs::remove(legacyPath);
}
