#include "Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h"

#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <unistd.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::alias;
using namespace lotus::unittest;
using PointsToSet = FlowSensitivePTA::PointsToSet;

class FlowSensitivePTATest : public LlvmModuleTest {};

TEST_F(FlowSensitivePTATest, MaintainsPerNodeMemoryInOutAndStrongUpdates) {
  auto module = parseModule(R"(
    @slot = global i8* null
    @x = global i8 0
    @y = global i8 0
    define i8* @f() {
    entry:
      store i8* @x, i8** @slot
      store i8* @y, i8** @slot
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);
  auto it = module->getFunction("f")->getEntryBlock().begin();
  auto *storeXInst = cast<StoreInst>(&*it++);
  auto *storeYInst = cast<StoreInst>(&*it++);
  auto *loadInst = cast<LoadInst>(&*it);

  constexpr uint32_t xObject = 100;
  constexpr uint32_t yObject = 101;
  constexpr uint32_t slotObject = 200;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *storeX = new StoreSVFGNode(3, nullptr, storeXInst, 0);
  storeX->setMemoryDef(1, 1, {slotObject});
  auto *storeY = new StoreSVFGNode(4, nullptr, storeYInst, 0);
  storeY->setMemoryDef(1, 2, {slotObject});
  auto *load = new LoadSVFGNode(5, nullptr, loadInst, 0);
  load->setMemoryUse(1, 2, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, storeX, storeY, load})
    graph.addNode(node);
  graph.setValueNode(module->getGlobalVariable("x"), x->getId());
  graph.setValueNode(module->getGlobalVariable("y"), y->getId());
  graph.setValueNode(loadInst, load->getId());
  SVFG::ObjectInfo singleton;
  singleton.isGlobal = true;
  singleton.isSingleton = true;
  graph.setObjectInfo(xObject, singleton);
  graph.setObjectInfo(yObject, singleton);
  graph.setObjectInfo(slotObject, singleton);
  graph.addEdge(x, storeX, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, storeY, SVFGEdgeK::IntraDirect);
  graph.addEdge(storeX, storeY, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});
  graph.addEdge(storeY, load, SVFGEdgeK::IntraIndirect, nullptr, {slotObject});

  FlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();
  EXPECT_EQ(solver.memoryOut(storeX, slotObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.memoryIn(storeY, slotObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.memoryOut(storeY, slotObject), PointsToSet({yObject}));
  EXPECT_EQ(solver.memoryIn(load, slotObject), PointsToSet({yObject}));
  EXPECT_EQ(solver.pointsTo(load), PointsToSet({yObject}));
  EXPECT_GT(stats.sccs, 0u);
  EXPECT_EQ(stats.strongUpdates, 2u);
  EXPECT_EQ(stats.weakUpdates, 0u);
  EXPECT_GE(stats.strongUpdateExecutions, stats.strongUpdates);

  FlowSensitivePTA::Config hashConfig;
  hashConfig.setBackend = PointsToSetBackend::HashConsed;
  FlowSensitivePTA hashSolver(graph, hashConfig);
  hashSolver.solve();
  EXPECT_EQ(hashSolver.pointsTo(load), solver.pointsTo(load));
  EXPECT_GT(hashSolver.statistics().hashConsedUniqueSets, 0u);
}

TEST_F(FlowSensitivePTATest, ThreadInterferenceProducesWeakMemoryJoin) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t yObject = 11;
  constexpr uint32_t slotObject = 20;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *first = new StoreSVFGNode(3, nullptr, nullptr, 0);
  first->setMemoryDef(1, 1, {slotObject});
  auto *second = new StoreSVFGNode(4, nullptr, nullptr, 0);
  second->setMemoryDef(1, 1, {slotObject});
  auto *load = new LoadSVFGNode(5, nullptr, nullptr, 0);
  load->setMemoryUse(1, 1, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, first, second, load})
    graph.addNode(node);
  graph.addEdge(x, first, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, second, SVFGEdgeK::IntraDirect);
  graph.addEdge(first, load, SVFGEdgeK::ThreadMHPIndirectVF, nullptr,
                {slotObject});
  graph.addEdge(second, load, SVFGEdgeK::ThreadMHPIndirectVF, nullptr,
                {slotObject});

  FlowSensitivePTA solver(graph);
  solver.solve();
  const PointsToSet expected{xObject, yObject};
  EXPECT_EQ(solver.memoryIn(load, slotObject), expected);
  EXPECT_EQ(solver.pointsTo(load), expected);
}

TEST_F(FlowSensitivePTATest, SolvesBuilderProducedSVFG) {
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

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool pointsToY = false;
  std::string observed;
  raw_string_ostream observedStream(observed);
  for (const auto &entry : *graph) {
    const auto *store = dyn_cast<StoreSVFGNode>(entry.second);
    if (!store)
      continue;
    observedStream << "store " << store->getId() << " targets=";
    if (const auto *storeInst =
            dyn_cast_or_null<StoreInst>(store->getInstruction())) {
      const Value *storedValue = storeInst->getValueOperand();
      const SVFGNode *storedNode = graph->getValueNode(storedValue);
      observedStream << " valueNode="
                     << (storedNode ? storedNode->getId() : 9999)
                     << " valuePts={";
      if (storedNode)
        for (uint32_t pointee : solver.pointsTo(storedNode))
          observedStream << pointee << ",";
      observedStream << "}; ";
    }
    for (uint32_t targetObject : store->getMemoryPointsTo()) {
      const SVFG::ObjectInfo *targetInfo = graph->getObjectInfo(targetObject);
      const Value *targetValue = graph->getObjectValue(targetObject);
      observedStream << " targetValue=";
      if (targetValue)
        observedStream << *targetValue;
      else
        observedStream << "<unknown>";
      observedStream << " constant=" << (targetInfo && targetInfo->isConstant)
                     << "; ";
      observedStream << targetObject << " out={";
      for (uint32_t pointee : solver.memoryOut(store, targetObject))
        observedStream << pointee << ",";
      observedStream << "}; ";
    }
  }
  observedStream << "load targets=";
  const auto *loadNode = dyn_cast<LoadSVFGNode>(graph->getDef(load));
  if (loadNode)
    for (uint32_t targetObject : loadNode->getMemoryPointsTo()) {
      observedStream << targetObject << " in={";
      for (uint32_t pointee : solver.memoryIn(loadNode, targetObject))
        observedStream << pointee << ",";
      observedStream << "}; ";
    }
  observedStream << "result=";
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    observedStream << object << "=";
    if (value)
      observedStream << *value;
    else
      observedStream << "<unknown>";
    observedStream << "; ";
    pointsToY |=
        value && value->stripPointerCasts() == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(pointsToY) << observedStream.str();
}

TEST_F(FlowSensitivePTATest, HashConsedBackendKeepsGlobalInitializerSets) {
  auto module = parseModule(R"(
    @x = global i8 0
    @slot = global i8* @x
    define i8* @main() {
    entry:
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const auto *load = dyn_cast<LoadInst>(
      &*module->getFunction("main")->getEntryBlock().begin());
  ASSERT_NE(load, nullptr);
  FlowSensitivePTA::Config config;
  config.setBackend = PointsToSetBackend::HashConsed;
  FlowSensitivePTA solver(*graph, config);
  solver.solve();

  auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    const Value *value = graph->getObjectValue(object);
    return value &&
           value->stripPointerCasts() == module->getGlobalVariable("x");
  }));
}

TEST_F(FlowSensitivePTATest, AggregateGlobalInitializersRespectFieldOffsets) {
  auto module = parseModule(R"(
    %G = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    @g = global %G { i8* @x, i8* @y }
    define i8* @main() {
    entry:
      %result = load i8*, i8** getelementptr (%G, %G* @g, i32 0, i32 0)
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const auto *load = dyn_cast<LoadInst>(
      &*module->getFunction("main")->getEntryBlock().begin());
  ASSERT_NE(load, nullptr);
  FlowSensitivePTA solver(*graph);
  solver.solve();

  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    hasX |= value == module->getGlobalVariable("x");
    hasY |= value == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(hasX);
  EXPECT_FALSE(hasY);
}

TEST_F(FlowSensitivePTATest, ConstantGepGlobalInitializerKeepsExactField) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    @aggregate = global %S { i8* @x, i8* @y }
    @field.pointer = global i8** getelementptr (%S, %S* @aggregate,
                                                i32 0, i32 1)
    define i8* @main() {
    entry:
      %field = load i8**, i8*** @field.pointer
      %result = load i8*, i8** %field
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *fieldLoad = nullptr;
  const LoadInst *resultLoad = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    const auto *load = dyn_cast<LoadInst>(&instruction);
    if (load && load->getName() == "field")
      fieldLoad = load;
    else if (load && load->getName() == "result")
      resultLoad = load;
  }
  ASSERT_NE(fieldLoad, nullptr);
  ASSERT_NE(resultLoad, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto fieldResult = solver.pointsTo(fieldLoad);
  ASSERT_TRUE(fieldResult.has_value());
  ASSERT_EQ(fieldResult->size(), 1u);
  const SVFG::ObjectInfo *fieldInfo =
      graph->getObjectInfo(*fieldResult->begin());
  ASSERT_NE(fieldInfo, nullptr);
  EXPECT_TRUE(fieldInfo->hasFieldOffset);
  EXPECT_EQ(fieldInfo->fieldOffset, 8u);

  const uint32_t aggregateObject =
      graph->getObjectId(module->getGlobalVariable("aggregate"));
  ASSERT_NE(aggregateObject, 0u);
  EXPECT_NE(*fieldResult->begin(), aggregateObject);
}

TEST_F(FlowSensitivePTATest, StrongUpdateDoesNotReintroduceGlobalInitializer) {
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
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    hasX |= value == module->getGlobalVariable("x");
    hasY |= value == module->getGlobalVariable("y");
  }
  EXPECT_FALSE(hasX);
  EXPECT_TRUE(hasY);
}

TEST_F(FlowSensitivePTATest, EmptyIndirectGuardDoesNotPropagateMemory) {
  auto module = parseModule(R"(
    @x = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t slotObject = 20;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {slotObject});
  auto *load = new LoadSVFGNode(3, nullptr, nullptr, 0);
  load->setMemoryUse(1, 1, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, store, load})
    graph.addNode(node);
  SVFG::ObjectInfo singleton;
  singleton.isSingleton = true;
  graph.setObjectInfo(slotObject, singleton);
  graph.addEdge(x, store, SVFGEdgeK::IntraDirect);
  graph.addEdge(store, load, SVFGEdgeK::IntraIndirect);

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_TRUE(solver.memoryIn(load, slotObject).empty());
  EXPECT_TRUE(solver.pointsTo(load).empty());
}

TEST_F(FlowSensitivePTATest, RefinedIndirectRecursionDisablesStrongUpdate) {
  auto module = parseModule(R"(
    @x = global i8 0
    define void @f(void ()* %fp) {
    entry:
      %slot = alloca i8*
      call void %fp()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  Function *function = module->getFunction("f");
  ASSERT_NE(function, nullptr);
  const auto *allocation =
      dyn_cast<AllocaInst>(&*function->getEntryBlock().begin());
  const auto *call =
      dyn_cast<CallBase>(&*std::next(function->getEntryBlock().begin()));
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(call, nullptr);

  constexpr uint32_t xObject = 10;
  constexpr uint32_t slotObject = 20;
  SVFG graph;
  graph.initializeRefinedCallGraph(*module);
  ASSERT_TRUE(graph.markConnectedCallee(call, function));
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {slotObject});
  graph.addNode(x);
  graph.addNode(store);
  graph.addEdge(x, store, SVFGEdgeK::IntraDirect);
  graph.setObjectValue(slotObject, allocation);
  SVFG::ObjectInfo stack;
  stack.isStack = true;
  stack.isSingleton = true;
  graph.setObjectInfo(slotObject, stack);

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_EQ(solver.memoryOut(store, slotObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.statistics().strongUpdates, 0u);
  EXPECT_GT(solver.statistics().weakUpdates, 0u);
}

TEST_F(FlowSensitivePTATest, MemcpyNullFieldKillsDestinationFact) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* null, i8** %source.field
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
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);
  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST_F(FlowSensitivePTATest, MemcpyUsesPointerSizeOfFieldAddressSpace) {
  auto module = parseModule(R"(
    target datalayout = "e-p:64:64-p1:32:32"
    %S = type { i8 addrspace(1)* }
    @x = addrspace(1) global i8 0
    define i8 addrspace(1)* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8 addrspace(1)* null, i8 addrspace(1)** %source.field
      store i8 addrspace(1)* @x, i8 addrspace(1)** %destination.field
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 4, i1 false)
      %result = load i8 addrspace(1)*, i8 addrspace(1)** %destination.field
      ret i8 addrspace(1)* %result
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST_F(FlowSensitivePTATest, PartialMemcpyPreservesFieldsOutsideRange) {
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
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *copied = nullptr;
  const LoadInst *preserved = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    const auto *load = dyn_cast<LoadInst>(&instruction);
    if (!load)
      continue;
    if (load->getName() == "copied")
      copied = load;
    else if (load->getName() == "preserved")
      preserved = load;
  }
  ASSERT_NE(copied, nullptr);
  ASSERT_NE(preserved, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  auto pointsToGlobal = [&](const LoadInst *load, StringRef name) {
    const auto result = solver.pointsTo(load);
    if (!result)
      return false;
    return std::any_of(result->begin(), result->end(), [&](uint32_t object) {
      return graph->getObjectValue(object) == module->getGlobalVariable(name);
    });
  };
  EXPECT_TRUE(pointsToGlobal(copied, "x"));
  EXPECT_FALSE(pointsToGlobal(copied, "z"));
  EXPECT_TRUE(pointsToGlobal(preserved, "w"));
  EXPECT_FALSE(pointsToGlobal(preserved, "y"));
}

TEST_F(FlowSensitivePTATest, PartialPointerOverwriteBecomesUnknown) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* null, i8** %source.field
      store i8* @x, i8** %destination.field
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 4, i1 false)
      %result = load i8*, i8** %destination.field
      ret i8* %result
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);
  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->isUnknownObject(object);
  }));
  EXPECT_FALSE(
      std::any_of(result->begin(), result->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == module->getGlobalVariable("x");
      }));
}

TEST_F(FlowSensitivePTATest, NegativeGepIndexDoesNotCreateUnsignedOffset) {
  auto module = parseModule(R"(
    define i8** @main(i8** %pointer) {
    entry:
      %previous = getelementptr i8*, i8** %pointer, i64 -1
      ret i8** %previous
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const auto *gep = dyn_cast<GetElementPtrInst>(
      &*module->getFunction("main")->getEntryBlock().begin());
  ASSERT_NE(gep, nullptr);
  EXPECT_FALSE(graph->getGepAccess(gep).valid);
}

TEST_F(FlowSensitivePTATest, FieldInsensitiveBaseDoesNotCreateFieldObject) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    define i8** @main() {
    entry:
      %object = alloca %S
      %field = getelementptr %S, %S* %object, i32 0, i32 1
      ret i8** %field
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.memModelType = SVFGBuilderConfig::MemModelType::FieldInsensitive;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const AllocaInst *allocation = nullptr;
  const GetElementPtrInst *gep = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    allocation = allocation ? allocation : dyn_cast<AllocaInst>(&instruction);
    gep = gep ? gep : dyn_cast<GetElementPtrInst>(&instruction);
  }
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(gep, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto base = solver.pointsTo(allocation);
  const auto field = solver.pointsTo(gep);
  ASSERT_TRUE(base.has_value());
  ASSERT_TRUE(field.has_value());
  EXPECT_EQ(*field, *base);
}

TEST_F(FlowSensitivePTATest, CanonicalGepOffsetsSeparateStructFields) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      %object = alloca %S
      %field0 = getelementptr %S, %S* %object, i32 0, i32 0
      %field1 = getelementptr %S, %S* %object, i32 0, i32 1
      store i8* @x, i8** %field0
      store i8* @y, i8** %field1
      %result = load i8*, i8** %field0
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const GetElementPtrInst *field0 = nullptr;
  const GetElementPtrInst *field1 = nullptr;
  const LoadInst *resultLoad = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    if (const auto *gep = dyn_cast<GetElementPtrInst>(&instruction)) {
      if (gep->getName() == "field0")
        field0 = gep;
      else if (gep->getName() == "field1")
        field1 = gep;
    } else if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
      resultLoad = load;
    }
  }
  ASSERT_NE(field0, nullptr);
  ASSERT_NE(field1, nullptr);
  ASSERT_NE(resultLoad, nullptr);
  EXPECT_EQ(graph->getValueNode(field0), graph->getDef(field0));
  EXPECT_EQ(graph->getValueNode(field1), graph->getDef(field1));

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(resultLoad);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->getObjectValue(object) == module->getGlobalVariable("x");
  }));
  EXPECT_FALSE(
      std::any_of(result->begin(), result->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == module->getGlobalVariable("y");
      }));
}

TEST_F(FlowSensitivePTATest, ArrayElementsUseWeakUpdates) {
  auto module = parseModule(R"(
    %A = type { [2 x i8*] }
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      %object = alloca %A
      %element0 = getelementptr %A, %A* %object, i32 0, i32 0, i64 0
      %element1 = getelementptr %A, %A* %object, i32 0, i32 0, i64 1
      store i8* @x, i8** %element0
      store i8* @y, i8** %element1
      %result = load i8*, i8** %element0
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *resultLoad = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *load = dyn_cast<LoadInst>(&instruction))
      resultLoad = load;
  ASSERT_NE(resultLoad, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(resultLoad);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    hasX |= value == module->getGlobalVariable("x");
    hasY |= value == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(hasX);
  EXPECT_TRUE(hasY);
  EXPECT_GT(solver.statistics().weakUpdates, 0u);
}

TEST_F(FlowSensitivePTATest, RestartsAfterOnTheFlyIndirectCallConnection) {
  auto module = parseModule(R"(
    define i8* @target(i8* %arg) { ret i8* %arg }
    define i8* @main(i8* %arg, i8* (i8*)* %fp) {
    entry:
      %result = call i8* %fp(i8* %arg)
      ret i8* %result
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

  constexpr uint32_t functionObject = 300;
  constexpr uint32_t valueID = 400;
  SVFG graph;
  auto *function = new AddrSVFGNode(1, nullptr, target, functionObject);
  function->setValueId(valueID);
  graph.addNode(function);
  graph.setObjectValue(functionObject, target);
  SVFG::ObjectInfo info;
  info.isFunction = true;
  info.isSingleton = true;
  graph.setObjectInfo(functionObject, info);
  graph.addIndCallSite(valueID, call);

  std::size_t connections = 0;
  FlowSensitivePTA::Config config;
  config.connectIndirectCall = [&](const CallBase *site,
                                   const Function *callee) {
    EXPECT_EQ(site, call);
    EXPECT_EQ(callee, target);
    ++connections;
    return connections == 1;
  };
  FlowSensitivePTA solver(graph, config);
  const auto &stats = solver.solve();
  EXPECT_EQ(connections, 1u);
  EXPECT_EQ(stats.indirectCallEdges, 1u);
}

TEST_F(FlowSensitivePTATest, IndirectGuardPropagatesFieldInsensitiveFields) {
  auto module = parseModule(R"(
    @x = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t baseObject = 20;
  constexpr uint32_t fieldObject = 21;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {fieldObject});
  auto *load = new LoadSVFGNode(3, nullptr, nullptr, 0);
  load->setMemoryUse(1, 1, {fieldObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, store, load})
    graph.addNode(node);
  SVFG::ObjectInfo base;
  base.isFieldInsensitive = true;
  base.baseObjId = baseObject;
  graph.setObjectInfo(baseObject, base);
  SVFG::ObjectInfo field;
  field.baseObjId = baseObject;
  graph.setObjectInfo(fieldObject, field);
  graph.addEdge(x, store, SVFGEdgeK::IntraDirect);
  graph.addEdge(store, load, SVFGEdgeK::IntraIndirect, nullptr, {baseObject});

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_EQ(solver.memoryIn(load, fieldObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.pointsTo(load), PointsToSet({xObject}));
}

TEST_F(FlowSensitivePTATest, HeapTargetsRemainWeakUpdates) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t yObject = 11;
  constexpr uint32_t heapObject = 20;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *first = new StoreSVFGNode(3, nullptr, nullptr, 0);
  first->setMemoryDef(1, 1, {heapObject});
  auto *second = new StoreSVFGNode(4, nullptr, nullptr, 0);
  second->setMemoryDef(1, 2, {heapObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, first, second})
    graph.addNode(node);
  SVFG::ObjectInfo heap;
  heap.isHeap = true;
  graph.setObjectInfo(heapObject, heap);
  graph.addEdge(x, first, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, second, SVFGEdgeK::IntraDirect);
  graph.addEdge(first, second, SVFGEdgeK::IntraIndirect, nullptr, {heapObject});

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_EQ(solver.memoryOut(second, heapObject),
            PointsToSet({xObject, yObject}));
  EXPECT_GT(solver.statistics().weakUpdates, 0u);
}

TEST_F(FlowSensitivePTATest, DeadStoreThroughBottomPointerDoesNotClobberMemory) {
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

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto pointsTo = solver.pointsTo(result);
  ASSERT_TRUE(pointsTo.has_value());
  ASSERT_EQ(pointsTo->size(), 1u);
  EXPECT_EQ(graph->getObjectValue(*pointsTo->begin()),
            module->getGlobalVariable("y"));
}

TEST_F(FlowSensitivePTATest, TimeLimitReportsTimeout) {
  SVFG graph;
  FlowSensitivePTA::Config config;
  config.timeLimitSeconds = -1; // already expired
  FlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_TRUE(solver.timedOut());
  EXPECT_TRUE(solver.statistics().timedOut);
  const std::string report = solver.statisticsReport();
  EXPECT_NE(report.find("timed out: yes"), std::string::npos);
  EXPECT_NE(report.find("svfgHasSU: no"), std::string::npos);
}

TEST_F(FlowSensitivePTATest, StatisticsReportDetectsStrongUpdateSites) {
  auto module = parseModule(R"(
    @x = global i8 0
    define void @f() {
    entry:
      %p = alloca i8*
      store i8* @x, i8** %p
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

  FlowSensitivePTA solver(*graph);
  solver.solve();
  EXPECT_TRUE(solver.statistics().svfgHasSU);
  const std::string report = solver.statisticsReport();
  EXPECT_NE(report.find("svfgHasSU: yes"), std::string::npos);
}

TEST_F(FlowSensitivePTATest, AnalysisResultRoundTripsThroughFile) {
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
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA solved(*graph);
  solved.solve();
  const auto pointsTo = solved.pointsTo(load);
  ASSERT_TRUE(pointsTo.has_value());

  SmallString<256> path;
  int fileDescriptor = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-sparsepta", "txt",
                                            fileDescriptor, path));
  ::close(fileDescriptor);
  ASSERT_TRUE(solved.writeAnalysisResultToFile(path.str().str()));

  FlowSensitivePTA restored(*graph);
  ASSERT_TRUE(restored.readAnalysisResultFromFile(path.str().str()));
  sys::fs::remove(path);

  EXPECT_EQ(restored.pointsTo(load), *pointsTo);
  const auto *loadNode = dyn_cast_or_null<LoadSVFGNode>(graph->getDef(load));
  ASSERT_NE(loadNode, nullptr);
  for (uint32_t object : *pointsTo)
    EXPECT_EQ(restored.memoryIn(loadNode, object),
              solved.memoryIn(loadNode, object));
}

TEST_F(FlowSensitivePTATest, RestoredSolutionReconnectsIndirectCallEdges) {
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
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-sparsepta-resume", "txt",
                                            fileDescriptor, path));
  ::close(fileDescriptor);

  FlowSensitivePTA::Config solvedConfig;
  solvedConfig.connectIndirectCall = connector(builder, graph.get());
  FlowSensitivePTA solved(*graph, std::move(solvedConfig));
  solved.solve();
  const auto solvedPointsTo = solved.pointsTo(result);
  ASSERT_TRUE(solvedPointsTo.has_value());
  ASSERT_FALSE(graph->getConnectedCallees(indirectCall).empty());
  ASSERT_TRUE(std::any_of(
      solvedPointsTo->begin(), solvedPointsTo->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == module->getGlobalVariable("y");
      }));
  ASSERT_TRUE(solved.writeAnalysisResultToFile(path.str().str()));

  // Resume into a fresh graph built without any indirect-call edges.
  ICFG resumeIcfg;
  ICFGBuilder resumeIcfgBuilder(&resumeIcfg);
  resumeIcfgBuilder.build(module.get());
  SVFGBuilder resumeBuilder(graphConfig);
  std::unique_ptr<SVFG> resumeGraph(resumeBuilder.build(&resumeIcfg));
  ASSERT_NE(resumeGraph, nullptr);
  ASSERT_TRUE(resumeGraph->getConnectedCallees(indirectCall).empty());

  FlowSensitivePTA::Config resumeConfig;
  resumeConfig.connectIndirectCall =
      connector(resumeBuilder, resumeGraph.get());
  FlowSensitivePTA restored(*resumeGraph, std::move(resumeConfig));
  ASSERT_TRUE(restored.readAnalysisResultFromFile(path.str().str()));
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

TEST_F(FlowSensitivePTATest, OnTheFlyBoundRejectsInfeasibleTargets) {
  auto module = parseModule(R"(
    define void @target() { ret void }
    define void @other() { ret void }
    define void @main(void ()** %fp) {
    entry:
      %f = load void ()*, void ()** %fp
      call void %f()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  const Function *target = module->getFunction("target");
  const Function *other = module->getFunction("other");
  const CallBase *call = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<CallBase>(&instruction))
      call = candidate;
  ASSERT_NE(target, nullptr);
  ASSERT_NE(other, nullptr);
  ASSERT_NE(call, nullptr);

  constexpr uint32_t targetObject = 300;
  constexpr uint32_t otherObject = 301;
  constexpr uint32_t functionPointerId = 400;
  SVFG graph;
  auto *targetAddress =
      new AddrSVFGNode(1, nullptr, target, targetObject);
  targetAddress->setValueId(functionPointerId);
  auto *otherAddress = new AddrSVFGNode(2, nullptr, other, otherObject);
  otherAddress->setValueId(functionPointerId);
  auto *phi = new PhiSVFGNode(3, SVFGK::IntraPhi, nullptr,
                              static_cast<const Value *>(nullptr));
  phi->setValueId(functionPointerId);
  for (SVFGNode *node : std::vector<SVFGNode *>{targetAddress, otherAddress,
                                                phi})
    graph.addNode(node);
  SVFG::ObjectInfo info;
  info.isFunction = true;
  info.isSingleton = true;
  graph.setObjectValue(targetObject, target);
  graph.setObjectInfo(targetObject, info);
  graph.setObjectValue(otherObject, other);
  graph.setObjectInfo(otherObject, info);
  graph.addEdge(targetAddress, phi, SVFGEdgeK::IntraDirect);
  graph.addEdge(otherAddress, phi, SVFGEdgeK::IntraDirect);
  graph.addIndCallSite(functionPointerId, call);

  FlowSensitivePTA::Config config;
  config.connectIndirectCall = [&](const CallBase *site,
                                   const Function *callee) {
    return graph.markConnectedCallee(site, callee);
  };
  config.isIndirectCallTargetAllowed = [&](const CallBase *,
                                           const Function *callee) {
    return callee == target;
  };
  FlowSensitivePTA solver(graph, std::move(config));
  const auto &stats = solver.solve();
  EXPECT_EQ(stats.indirectCallEdges, 1u);
  EXPECT_GE(stats.indirectCallsRejected, 1u);
  EXPECT_EQ(stats.unresolvedIndirectCalls, 0u);
  EXPECT_TRUE(graph.hasConnectedCallee(call, target));
  EXPECT_FALSE(graph.hasConnectedCallee(call, other));
}

TEST_F(FlowSensitivePTATest, MissingConnectorCountsUnresolvedIndirectCalls) {
  auto module = parseModule(R"(
    define void @target() { ret void }
    define void @main(void ()** %fp) {
    entry:
      %f = load void ()*, void ()** %fp
      call void %f()
      ret void
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

  constexpr uint32_t targetObject = 300;
  constexpr uint32_t functionPointerId = 400;
  SVFG graph;
  auto *targetAddress =
      new AddrSVFGNode(1, nullptr, target, targetObject);
  targetAddress->setValueId(functionPointerId);
  graph.addNode(targetAddress);
  SVFG::ObjectInfo info;
  info.isFunction = true;
  info.isSingleton = true;
  graph.setObjectValue(targetObject, target);
  graph.setObjectInfo(targetObject, info);
  graph.addIndCallSite(functionPointerId, call);

  FlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();
  EXPECT_EQ(stats.indirectCallEdges, 0u);
  EXPECT_EQ(stats.indirectCallsRejected, 0u);
  EXPECT_EQ(stats.unresolvedIndirectCalls, 1u);
  EXPECT_NE(solver.statisticsReport().find("unresolved indirect callsites"),
            std::string::npos);
}

TEST_F(FlowSensitivePTATest, UnannotatedGepCollapsesToFieldInsensitiveObject) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    define i8** @f(%S* %base) {
    entry:
      %gep = getelementptr %S, %S* %base, i32 0, i32 1
      ret i8** %gep
    }
  )");
  ASSERT_NE(module, nullptr);
  const auto *gep = dyn_cast<GetElementPtrInst>(
      module->getFunction("f")->getEntryBlock().getFirstNonPHI());
  ASSERT_NE(gep, nullptr);

  // A hand-built (or deserialized) graph may carry no GepAccessInfo for a
  // GEP. That is the same condition under which a variant field access
  // reaches the solver, so the transfer must collapse the struct instead
  // of keeping per-field objects.
  constexpr uint32_t baseObject = 500;
  SVFG graph;
  auto *base = new AddrSVFGNode(1, nullptr, module->getFunction("f")->getArg(0),
                                baseObject);
  auto *gepNode = new GepSVFGNode(2, nullptr, gep);
  graph.addNode(base);
  graph.addNode(gepNode);
  SVFG::ObjectInfo info;
  info.isStack = true;
  info.baseObjId = baseObject;
  graph.setObjectInfo(baseObject, info);
  graph.setObjectValue(baseObject, module->getFunction("f")->getArg(0));
  graph.addEdge(base, gepNode, SVFGEdgeK::IntraDirect);

  FlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();
  const auto &result = solver.pointsTo(gepNode);
  ASSERT_EQ(result.size(), 1u);
  const uint32_t collapsed = *result.begin();
  EXPECT_NE(collapsed, baseObject);
  EXPECT_TRUE(graph.isFieldInsensitiveObject(baseObject));
  const SVFG::ObjectInfo *collapsedInfo = graph.getObjectInfo(collapsed);
  ASSERT_NE(collapsedInfo, nullptr);
  EXPECT_TRUE(collapsedInfo->isFieldInsensitive);
  EXPECT_FALSE(collapsedInfo->hasFieldOffset);
  EXPECT_EQ(collapsedInfo->baseObjId, baseObject);
  EXPECT_EQ(stats.fieldInsensitiveCollapses, 1u);
}

TEST_F(FlowSensitivePTATest, StatisticsReportPerNodeKindAndPointsToSizes) {
  auto module = parseModule(R"(
    @x = global i8 0
    define i8* @f() {
    entry:
      %p = alloca i8*
      store i8* @x, i8** %p
      %q = load i8*, i8** %p
      ret i8* %q
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  FlowSensitivePTA solver(*graph);
  const auto &stats = solver.solve();
  EXPECT_GE(stats.solveIterations, 1u);
  EXPECT_GE(stats.processedAddr, 1u);
  EXPECT_GE(stats.processedLoad, 1u);
  EXPECT_GE(stats.processedStore, 1u);
  EXPECT_GE(stats.maxTopLevelPtsSize, 1u);
  EXPECT_GT(stats.averageTopLevelPtsSize, 0.0);
  EXPECT_LE(stats.averageTopLevelPtsSize,
            static_cast<double>(stats.maxTopLevelPtsSize));
  const std::string report = solver.statisticsReport();
  EXPECT_NE(report.find("processed addr/copy/gep/load/store/phi"),
            std::string::npos);
  EXPECT_NE(report.find("top-level pts size max/avg"), std::string::npos);
}

TEST_F(FlowSensitivePTATest, PointerOnlyModeSkipsScalarStoreStrongUpdates) {
  auto module = parseModule(R"(
    @n = global i32 0
    define void @f() {
    entry:
      store i32 7, i32* @n
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  const auto *scalarStore = dyn_cast<StoreInst>(
      module->getFunction("f")->getEntryBlock().getFirstNonPHI());
  ASSERT_NE(scalarStore, nullptr);
  ASSERT_FALSE(scalarStore->getValueOperand()->getType()->isPointerTy());

  constexpr uint32_t object = 300;
  SVFG graph;
  auto *store = new StoreSVFGNode(1, nullptr, scalarStore, 0);
  store->setMemoryDef(1, 1, {object});
  graph.addNode(store);
  SVFG::ObjectInfo info;
  info.isGlobal = true;
  graph.setObjectInfo(object, info);

  // SVF's pointer-only SVFG has no node for this store, so it must not kill
  // the (pointer) contents of its destination.
  FlowSensitivePTA pointerOnly(graph);
  const auto &stats = pointerOnly.solve();
  EXPECT_EQ(stats.strongUpdates, 0u);
  EXPECT_TRUE(pointerOnly.memoryOut(store, object).empty());

  // The pre-parity behaviour processes the scalar store and applies a strong
  // update with an empty points-to set.
  FlowSensitivePTA::Config legacyConfig;
  legacyConfig.pointerOnlySVFG = false;
  FlowSensitivePTA legacy(graph, legacyConfig);
  const auto &legacyStats = legacy.solve();
  EXPECT_EQ(legacyStats.strongUpdates, 1u);
}

TEST_F(FlowSensitivePTATest,
       TypeClusteringDisablesStrongUpdatesAndPreservesAliases) {
  auto module = parseModule(R"(
    @x = global i8 0
    define i8* @f() {
    entry:
      %a = alloca i8*
      %b = alloca i8*
      store i8* @x, i8** %a
      %result = load i8*, i8** %a
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
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction : instructions(module->getFunction("f")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA plain(*graph);
  const auto &plainStats = plain.solve();
  EXPECT_GE(plainStats.strongUpdates, 1u);
  const auto plainResult = plain.pointsTo(load);
  ASSERT_TRUE(plainResult.has_value());

  FlowSensitivePTA::Config clusteredConfig;
  clusteredConfig.clusterObjects =
      FlowSensitivePTA::Config::ClusterMethod::Type;
  FlowSensitivePTA clustered(*graph, clusteredConfig);
  const auto &clusteredStats = clustered.solve();
  EXPECT_GT(clusteredStats.clusteredObjects, 0u);
  EXPECT_GT(clusteredStats.mergedObjectClusters, 0u);
  // The two `i8*` slots are one class, so the store must not kill the other
  // member's contents.
  EXPECT_EQ(clusteredStats.strongUpdates, 0u);
  EXPECT_EQ(clustered.pointsTo(load), plainResult);
  EXPECT_EQ(clustered.mayAlias(load, module->getGlobalVariable("x")),
            plain.mayAlias(load, module->getGlobalVariable("x")));
}

TEST_F(FlowSensitivePTATest, FieldInsensitiveIdentitySurvivesResultFile) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    define i8** @f(%S* %base) {
    entry:
      %gep = getelementptr %S, %S* %base, i32 0, i32 1
      ret i8** %gep
    }
  )");
  ASSERT_NE(module, nullptr);
  const auto *gep = dyn_cast<GetElementPtrInst>(
      module->getFunction("f")->getEntryBlock().getFirstNonPHI());
  ASSERT_NE(gep, nullptr);

  constexpr uint32_t baseObject = 500;
  SVFG graph;
  auto *base = new AddrSVFGNode(1, nullptr, module->getFunction("f")->getArg(0),
                                baseObject);
  auto *gepNode = new GepSVFGNode(2, nullptr, gep);
  graph.addNode(base);
  graph.addNode(gepNode);
  SVFG::ObjectInfo info;
  info.isStack = true;
  info.baseObjId = baseObject;
  graph.setObjectInfo(baseObject, info);
  graph.setObjectValue(baseObject, module->getFunction("f")->getArg(0));
  graph.addEdge(base, gepNode, SVFGEdgeK::IntraDirect);

  FlowSensitivePTA solved(graph);
  solved.solve();
  const auto &result = solved.pointsTo(gepNode);
  ASSERT_EQ(result.size(), 1u);
  const uint32_t collapsed = *result.begin();
  ASSERT_NE(collapsed, baseObject);
  // The dedicated field-insensitive representative reports its base object as
  // its identity; SVF has no such extra object in the first place.
  EXPECT_EQ(solved.canonicalObject(collapsed), baseObject);
  EXPECT_EQ(solved.canonicalObject(baseObject), baseObject);

  SmallString<256> path;
  int fileDescriptor = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-sparsepta-fi", "txt",
                                            fileDescriptor, path));
  ::close(fileDescriptor);
  ASSERT_TRUE(solved.writeAnalysisResultToFile(path.str().str()));
  FlowSensitivePTA restored(graph);
  ASSERT_TRUE(restored.readAnalysisResultFromFile(path.str().str()));
  sys::fs::remove(path);
  EXPECT_EQ(restored.canonicalObject(collapsed), baseObject);
}

TEST_F(FlowSensitivePTATest, TypeClusteringOnlyAddsPointsToFacts) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i16 0
    define i8* @f() {
    entry:
      %a = alloca i8*
      %b = alloca i8*
      %cast = bitcast i16* @y to i8*
      store i8* @x, i8** %a
      store i8* %cast, i8** %a
      %result = load i8*, i8** %a
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder(&icfg).build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction : instructions(module->getFunction("f")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA plain(*graph);
  plain.solve();
  const auto plainResult = plain.pointsTo(load);
  ASSERT_TRUE(plainResult.has_value());
  ASSERT_EQ(plainResult->size(), 1u);

  FlowSensitivePTA::Config clusteredConfig;
  clusteredConfig.clusterObjects =
      FlowSensitivePTA::Config::ClusterMethod::Type;
  FlowSensitivePTA clustered(*graph, clusteredConfig);
  const auto &clusteredStats = clustered.solve();
  const auto clusteredResult = clustered.pointsTo(load);
  ASSERT_TRUE(clusteredResult.has_value());
  ASSERT_GT(clusteredStats.mergedObjectClusters, 0u);

  // %a and the unused %b form one class, so the second store becomes a weak
  // update. Coarsening may add facts; it must never remove a mapped plain one.
  EXPECT_GT(clusteredResult->size(), plainResult->size());
  const auto mappedPlain = clustered.objectClustering().mapAll(*plainResult);
  for (FlowSensitivePTA::ObjectID object : mappedPlain)
    EXPECT_NE(clusteredResult->find(object), clusteredResult->end());
}
