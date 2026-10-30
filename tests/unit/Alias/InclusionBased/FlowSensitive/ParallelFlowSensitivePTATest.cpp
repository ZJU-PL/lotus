#include "Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <random>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::alias;
using namespace lotus::analysis;
using namespace lotus::unittest;

class ParallelFlowSensitivePTATest : public LlvmModuleTest {
protected:
  void compare(SVFG &graph, const FilteredSVFGView *scope = nullptr) {
    FlowSensitivePTA::Config sequential;
    sequential.scope = scope;
    FlowSensitivePTA reference(graph, sequential);
    reference.solve();
    EXPECT_TRUE(reference.isFixedPoint());
    for (unsigned threads : {1U, 2U, 4U}) {
      for (auto backend :
           {PointsToSetBackend::Mutable, PointsToSetBackend::HashConsed}) {
        for (bool share : {false, true}) {
          for (bool objects : {false, true}) {
            FlowSensitivePTA::Config config = sequential;
            config.parallel = true;
            config.workerThreads = threads;
            config.setBackend = backend;
            config.shareParallelSets = share;
            config.parallelObjectCertificates = objects;
            FlowSensitivePTA parallel(graph, config);
            parallel.solve();
            EXPECT_TRUE(parallel.hasSameSolution(reference))
                << "threads=" << threads
                << " backend=" << static_cast<int>(backend);
            EXPECT_TRUE(parallel.isFixedPoint());
            EXPECT_EQ(parallel.statistics().parallelCommits,
                      reference.statistics().nodeProcesses);
            EXPECT_GT(parallel.statistics().parallelCommits, 0U);
            parallel.solve();
            EXPECT_TRUE(parallel.hasSameSolution(reference))
                << "repeated solve";
          }
        }
      }
    }
  }
  void compareIR(StringRef ir) {
    auto module = parseModule(ir.str());
    ASSERT_NE(module, nullptr);
    ICFG icfg;
    ICFGBuilder(&icfg).build(module.get());
    SVFGBuilderConfig config;
    config.usePointerAnalysis = true;
    config.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;
    SVFGBuilder builder(config);
    std::unique_ptr<SVFG> graph(builder.build(&icfg));
    ASSERT_NE(graph, nullptr);
    compare(*graph);
  }
};

TEST_F(ParallelFlowSensitivePTATest, StrongUpdatesAndFieldInitializers) {
  compareIR(R"(
    @x = global i8 0
    @y = global i8 0
    @pair = global {i8*, i8*} {i8* @x, i8* @y}
    define i8* @main() {
      %a = getelementptr {i8*, i8*}, {i8*, i8*}* @pair, i64 0, i32 0
      %b = getelementptr {i8*, i8*}, {i8*, i8*}* @pair, i64 0, i32 1
      store i8* @y, i8** %a
      store i8* null, i8** %b
      %r = load i8*, i8** %a
      ret i8* %r
    }
  )");
}

TEST_F(ParallelFlowSensitivePTATest,
       GrowingStoreTargetsInvalidateStrongUpdates) {
  compareIR(R"(
    @x = global i8 0
    @y = global i8 0
    @a = global i8* @x
    @b = global i8* @y
    define i8* @main(i1 %choice) {
      %slot = select i1 %choice, i8** @a, i8** @b
      store i8* null, i8** %slot
      %r = load i8*, i8** @a
      ret i8* %r
    }
  )");
}

TEST_F(ParallelFlowSensitivePTATest, IntrinsicCopiesAndLoops) {
  compareIR(R"(
    @x = global i8 0
    @source = global {i8*, i8*} {i8* @x, i8* null}
    @dest = global {i8*, i8*} zeroinitializer
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    define i8* @main(i1 %again) {
    entry:
      %s = bitcast {i8*, i8*}* @source to i8*
      %d = bitcast {i8*, i8*}* @dest to i8*
      br label %loop
    loop:
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %d, i8* %s, i64 16, i1 false)
      br i1 %again, label %loop, label %exit
    exit:
      %field = getelementptr {i8*, i8*}, {i8*, i8*}* @dest, i64 0, i32 0
      %r = load i8*, i8** %field
      ret i8* %r
    }
  )");
}

TEST_F(ParallelFlowSensitivePTATest, RecursiveCallsAndMemoryFlow) {
  compareIR(R"(
    @x = global i8 0
    @slot = global i8* null
    define void @recur(i1 %stop) {
    entry:
      %local = alloca i8*
      store i8* @x, i8** %local
      br i1 %stop, label %exit, label %recursive
    recursive:
      call void @recur(i1 true)
      br label %exit
    exit:
      %p = load i8*, i8** %local
      store i8* %p, i8** @slot
      ret void
    }
    define i8* @main() {
      call void @recur(i1 false)
      %r = load i8*, i8** @slot
      ret i8* %r
    }
  )");
}

TEST_F(ParallelFlowSensitivePTATest, RandomCyclicValueAndMemoryGraphs) {
  std::mt19937 random(0x50495045);
  for (unsigned seed = 0; seed < 24; ++seed) {
    SCOPED_TRACE(seed);
    SVFG graph;
    std::vector<SVFGNode *> values, stores, loads;
    uint32_t nextID = 1;
    SVFG::ObjectInfo info;
    info.isGlobal = true;
    info.isSingleton = true;
    for (unsigned i = 0; i < 6; ++i) {
      auto *node = new AddrSVFGNode(nextID++, nullptr, nullptr, 100 + i);
      graph.addNode(node);
      values.push_back(node);
      graph.setObjectInfo(100 + i, info);
      graph.setObjectInfo(200 + i, info);
    }
    for (unsigned i = 0; i < 18; ++i) {
      auto *node = new CopySVFGNode(nextID++, nullptr, nullptr);
      graph.addNode(node);
      graph.addEdge(values[random() % values.size()], node,
                    SVFGEdgeK::IntraDirect);
      values.push_back(node);
    }
    for (unsigned i = 0; i < 12; ++i) {
      auto *store = new StoreSVFGNode(nextID++, nullptr, nullptr, 0);
      auto *load = new LoadSVFGNode(nextID++, nullptr, nullptr, 0);
      const uint32_t object = 200 + random() % 6;
      store->setMemoryDef(1, i + 1, {object});
      load->setMemoryUse(1, i + 1, {object});
      graph.addNode(store);
      graph.addNode(load);
      graph.addEdge(values[random() % values.size()], store,
                    SVFGEdgeK::IntraDirect);
      graph.addEdge(store, load, SVFGEdgeK::IntraIndirect, nullptr, {object});
      graph.addEdge(load, values[6 + random() % 18], SVFGEdgeK::IntraDirect);
      stores.push_back(store);
      loads.push_back(load);
    }
    for (unsigned i = 1; i < stores.size(); ++i)
      graph.addEdge(stores[i - 1], stores[i], SVFGEdgeK::IntraIndirect, nullptr,
                    {200, 201, 202, 203, 204, 205});
    compare(graph);
  }
}

TEST_F(ParallelFlowSensitivePTATest, TopologyChangesRunOnTheCallingThread) {
  auto module = parseModule(R"(
    @x = global i8 0
    define i8* @target() { ret i8* @x }
    define i8* @main(i8* ()* %fp) {
      %r = call i8* %fp()
      ret i8* %r
    }
  )");
  ASSERT_NE(module, nullptr);
  SVFG graph;
  const auto *function = module->getFunction("target");
  auto *address = new AddrSVFGNode(1, nullptr, function, 100);
  graph.addNode(address);
  graph.setObjectValue(100, function);
  const auto *call =
      cast<CallBase>(&*module->getFunction("main")->front().begin());
  graph.addIndCallSite(1, call);
  const auto caller = std::this_thread::get_id();
  bool connected = false;
  FlowSensitivePTA::Config config;
  config.parallel = true;
  config.workerThreads = 4;
  config.connectIndirectCall = [&](const CallBase *, const Function *) {
    EXPECT_EQ(std::this_thread::get_id(), caller);
    if (connected)
      return false;
    connected = true;
    auto *copy = new CopySVFGNode(2, nullptr, nullptr);
    graph.addNode(copy);
    graph.addEdge(address, copy, SVFGEdgeK::IntraDirect);
    return true;
  };
  FlowSensitivePTA parallel(graph, config);
  parallel.solve();
  EXPECT_TRUE(connected);
  EXPECT_EQ(parallel.statistics().topologyEpochs, 2U);
  EXPECT_EQ(parallel.pointsTo(graph.getNode(2)),
            FlowSensitivePTA::PointsToSet{100});
  FlowSensitivePTA reference(graph);
  reference.solve();
  EXPECT_TRUE(parallel.hasSameSolution(reference));
}

TEST_F(ParallelFlowSensitivePTATest, EmptyGraph) {
  SVFG graph;
  FlowSensitivePTA::Config config;
  config.parallel = true;
  config.workerThreads = 4;
  FlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_EQ(solver.statistics().nodes, 0U);
  EXPECT_EQ(solver.statistics().parallelThreads, 0U);
}

TEST_F(ParallelFlowSensitivePTATest, FilteredScope) {
  SVFG graph;
  auto *address = new AddrSVFGNode(1, nullptr, nullptr, 100);
  auto *kept = new CopySVFGNode(2, nullptr, nullptr);
  auto *dropped = new CopySVFGNode(3, nullptr, nullptr);
  for (SVFGNode *node : std::vector<SVFGNode *>{address, kept, dropped})
    graph.addNode(node);
  graph.addEdge(address, kept, SVFGEdgeK::IntraDirect);
  graph.addEdge(kept, dropped, SVFGEdgeK::IntraDirect);
  FilteredSVFGView view(graph, FilteredSVFGView::NodeSet{address, kept});
  compare(graph, &view);
}

TEST_F(ParallelFlowSensitivePTATest, ConnectorFailureJoinsWorkers) {
  auto module = parseModule(R"(
    define void @target() { ret void }
    define void @main(void ()* %fp) { call void %fp() ret void }
  )");
  ASSERT_NE(module, nullptr);
  SVFG graph;
  const Function *target = module->getFunction("target");
  auto *address = new AddrSVFGNode(1, nullptr, target, 100);
  graph.addNode(address);
  graph.setObjectValue(100, target);
  const auto *call =
      cast<CallBase>(&*module->getFunction("main")->front().begin());
  graph.addIndCallSite(1, call);
  for (uint32_t id = 2; id < 100; ++id) {
    auto *node = new CopySVFGNode(id, nullptr, nullptr);
    graph.addNode(node);
    graph.addEdge(address, node, SVFGEdgeK::IntraDirect);
  }
  FlowSensitivePTA::Config config;
  config.parallel = true;
  config.workerThreads = 4;
  config.connectIndirectCall = [](const CallBase *, const Function *) -> bool {
    throw std::runtime_error("connector failed");
  };
  FlowSensitivePTA failing(graph, config);
  EXPECT_THROW(failing.solve(), std::runtime_error);
  config.connectIndirectCall = {};
  FlowSensitivePTA recovered(graph, config);
  recovered.solve();
  FlowSensitivePTA reference(graph);
  reference.solve();
  EXPECT_TRUE(recovered.hasSameSolution(reference));
}

TEST_F(ParallelFlowSensitivePTATest, CallbackOrderMatchesReference) {
  auto module = parseModule(R"(
    define void @first() { ret void }
    define void @second() { ret void }
    define void @main(void ()* %fp) {
      call void %fp()
      call void %fp()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  SVFG graph;
  auto *first = new AddrSVFGNode(1, nullptr, module->getFunction("first"), 100);
  auto *second =
      new AddrSVFGNode(2, nullptr, module->getFunction("second"), 101);
  auto *pointer = new CopySVFGNode(3, nullptr, nullptr);
  pointer->setValueId(900);
  graph.setObjectValue(100, module->getFunction("first"));
  graph.setObjectValue(101, module->getFunction("second"));
  for (SVFGNode *node : std::vector<SVFGNode *>{first, second, pointer})
    graph.addNode(node);
  graph.addEdge(first, pointer, SVFGEdgeK::IntraDirect);
  graph.addEdge(second, pointer, SVFGEdgeK::IntraDirect);
  for (const Instruction &instruction : module->getFunction("main")->front())
    if (const auto *call = dyn_cast<CallBase>(&instruction))
      graph.addIndCallSite(900, call);
  using Connection = std::pair<const CallBase *, const Function *>;
  std::vector<Connection> expected, actual;
  FlowSensitivePTA::Config config;
  config.connectIndirectCall = [&](const CallBase *site,
                                   const Function *target) {
    expected.emplace_back(site, target);
    return false;
  };
  FlowSensitivePTA reference(graph, config);
  reference.solve();
  config.parallel = true;
  config.workerThreads = 4;
  config.connectIndirectCall = [&](const CallBase *site,
                                   const Function *target) {
    actual.emplace_back(site, target);
    return false;
  };
  FlowSensitivePTA parallel(graph, config);
  parallel.solve();
  EXPECT_FALSE(expected.empty());
  EXPECT_EQ(actual, expected);
  EXPECT_TRUE(parallel.hasSameSolution(reference));
}

TEST_F(ParallelFlowSensitivePTATest, BlockSizesPreserveRetirementTrace) {
  SVFG graph;
  auto *address = new AddrSVFGNode(1, nullptr, nullptr, 100);
  graph.addNode(address);
  SVFGNode *previous = address;
  for (uint32_t id = 2; id <= 96; ++id) {
    auto *node = new CopySVFGNode(id, nullptr, nullptr);
    graph.addNode(node);
    graph.addEdge(previous, node, SVFGEdgeK::IntraDirect);
    if (id > 8)
      graph.addEdge(node, graph.getNode(id - 7), SVFGEdgeK::IntraDirect);
    previous = node;
  }
  FlowSensitivePTA reference(graph);
  reference.solve();
  for (unsigned size : {1U, 2U, 8U, 32U}) {
    SCOPED_TRACE(size);
    FlowSensitivePTA::Config config;
    config.parallel = true;
    config.workerThreads = 4;
    config.parallelBlockSize = size;
    FlowSensitivePTA parallel(graph, config);
    parallel.solve();
    EXPECT_TRUE(parallel.hasSameSolution(reference));
    EXPECT_TRUE(parallel.isFixedPoint());
    EXPECT_EQ(parallel.statistics().parallelCommits,
              reference.statistics().nodeProcesses);
    EXPECT_LE(parallel.statistics().parallelPeakWorkers, 4U);
  }
}

TEST_F(ParallelFlowSensitivePTATest, MissingPreciseEntryTracksWildcardBirth) {
  SVFG graph;
  auto *value = new AddrSVFGNode(1, nullptr, nullptr, 100);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 0);
  auto *load = new LoadSVFGNode(3, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {500});
  load->setMemoryUse(1, 1, {200});
  for (SVFGNode *node : std::vector<SVFGNode *>{value, store, load})
    graph.addNode(node);
  SVFG::ObjectInfo unknown;
  unknown.isUnknown = true;
  graph.setObjectInfo(500, unknown);
  graph.addEdge(value, store, SVFGEdgeK::IntraDirect);
  graph.addEdge(store, load, SVFGEdgeK::IntraIndirect, nullptr, {200});
  compare(graph);
  FlowSensitivePTA::Config config;
  config.parallel = true;
  config.workerThreads = 1;
  FlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_EQ(solver.pointsTo(load), FlowSensitivePTA::PointsToSet{100});
  EXPECT_GT(solver.statistics().parallelNegativeReads, 0U);
  EXPECT_GT(solver.statistics().parallelWildcardReads, 0U);
}

TEST_F(ParallelFlowSensitivePTATest,
       ExplicitEmptyEntrySuppressesWildcardFallback) {
  auto module = parseModule(R"(
    @slot = global i8* null
    @x = global i8 0
    define void @f() {
      store i8* null, i8** @slot
      %r = load i8*, i8** @slot
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  const auto *storeInstruction =
      cast<StoreInst>(&*module->getFunction("f")->front().begin());
  const auto *loadInstruction = cast<LoadInst>(storeInstruction->getNextNode());
  SVFG graph;
  auto *slot =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("slot"), 200);
  auto *value =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("x"), 100);
  auto *unknownStore = new StoreSVFGNode(3, nullptr, nullptr, 0);
  auto *emptyStore = new StoreSVFGNode(4, nullptr, storeInstruction, 0);
  auto *merge = new CopySVFGNode(5, nullptr, nullptr);
  auto *load = new LoadSVFGNode(6, nullptr, loadInstruction, 0);
  unknownStore->setMemoryDef(1, 1, {500});
  emptyStore->setMemoryDef(1, 2, {200});
  load->setMemoryUse(1, 2, {200});
  for (SVFGNode *node : std::vector<SVFGNode *>{slot, value, unknownStore,
                                                emptyStore, merge, load})
    graph.addNode(node);
  graph.setValueNode(module->getGlobalVariable("slot"), 1);
  graph.setValueNode(module->getGlobalVariable("x"), 2);
  graph.setObjectValue(200, module->getGlobalVariable("slot"));
  graph.setObjectValue(100, module->getGlobalVariable("x"));
  SVFG::ObjectInfo singleton;
  singleton.isGlobal = true;
  singleton.isSingleton = true;
  graph.setObjectInfo(200, singleton);
  graph.setObjectInfo(100, singleton);
  SVFG::ObjectInfo unknown;
  unknown.isUnknown = true;
  graph.setObjectInfo(500, unknown);
  graph.addEdge(value, unknownStore, SVFGEdgeK::IntraDirect);
  graph.addEdge(slot, emptyStore, SVFGEdgeK::IntraDirect);
  graph.addEdge(unknownStore, emptyStore, SVFGEdgeK::IntraIndirect, nullptr,
                {500});
  graph.addEdge(emptyStore, merge, SVFGEdgeK::IntraIndirect, nullptr,
                {200, 500});
  graph.addEdge(merge, load, SVFGEdgeK::IntraIndirect, nullptr, {200});
  compare(graph);
  FlowSensitivePTA::Config config;
  config.parallel = true;
  config.workerThreads = 4;
  FlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_TRUE(solver.pointsTo(load).empty());
  EXPECT_TRUE(solver.memoryIn(load, 200).empty());
  EXPECT_EQ(solver.memoryOut(emptyStore, 500),
            FlowSensitivePTA::PointsToSet{100});
}

TEST_F(ParallelFlowSensitivePTATest, PreciseGuardIgnoresOtherMemoryObjects) {
  SVFG graph;
  auto *x = new AddrSVFGNode(1, nullptr, nullptr, 100);
  auto *y = new AddrSVFGNode(2, nullptr, nullptr, 101);
  auto *first = new StoreSVFGNode(3, nullptr, nullptr, 0);
  auto *second = new StoreSVFGNode(4, nullptr, nullptr, 0);
  auto *load = new LoadSVFGNode(5, nullptr, nullptr, 0);
  first->setMemoryDef(1, 1, {200});
  second->setMemoryDef(1, 2, {201});
  load->setMemoryUse(1, 2, {200});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, first, second, load})
    graph.addNode(node);
  SVFG::ObjectInfo singleton;
  singleton.isGlobal = true;
  singleton.isSingleton = true;
  graph.setObjectInfo(200, singleton);
  graph.setObjectInfo(201, singleton);
  graph.addEdge(x, first, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, second, SVFGEdgeK::IntraDirect);
  graph.addEdge(first, second, SVFGEdgeK::IntraIndirect, nullptr, {200});
  graph.addEdge(second, load, SVFGEdgeK::IntraIndirect, nullptr, {200});
  compare(graph);
  FlowSensitivePTA::Config config;
  config.parallel = true;
  config.workerThreads = 4;
  FlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_EQ(solver.pointsTo(load), FlowSensitivePTA::PointsToSet{100});
  EXPECT_GT(solver.statistics().parallelObjectReads, 0U);
}

TEST_F(ParallelFlowSensitivePTATest, WildcardRetractionInvalidatesMissingEntryRead) {
  auto module = parseModule(R"(
    @slot = global i8* null
    @x = global i8 0
    define void @f() { store i8* @x, i8** @slot ret void }
  )");
  ASSERT_NE(module, nullptr);
  const auto *instruction = cast<StoreInst>(&*module->getFunction("f")->front().begin());
  SVFG graph;
  auto *value = new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), 100);
  auto *slot = new AddrSVFGNode(2, nullptr, module->getGlobalVariable("slot"), 200);
  auto *store = new StoreSVFGNode(3, nullptr, instruction, 0);
  auto *load = new LoadSVFGNode(4, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {500});
  load->setMemoryUse(1, 1, {201});
  for (SVFGNode *node : std::vector<SVFGNode *>{value, slot, store, load})
    graph.addNode(node);
  graph.setValueNode(module->getGlobalVariable("x"), 1);
  graph.setValueNode(module->getGlobalVariable("slot"), 2);
  graph.setObjectValue(100, module->getGlobalVariable("x"));
  graph.setObjectValue(200, module->getGlobalVariable("slot"));
  SVFG::ObjectInfo singleton;
  singleton.isGlobal = true;
  singleton.isSingleton = true;
  graph.setObjectInfo(200, singleton);
  SVFG::ObjectInfo unknown;
  unknown.isUnknown = true;
  graph.setObjectInfo(500, unknown);
  graph.addEdge(value, store, SVFGEdgeK::IntraDirect);
  graph.addEdge(slot, store, SVFGEdgeK::IntraDirect);
  graph.addEdge(store, load, SVFGEdgeK::IntraIndirect, nullptr, {201});
  compare(graph);
  FlowSensitivePTA::Config config;
  config.parallel = true;
  config.workerThreads = 4;
  FlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_TRUE(solver.pointsTo(load).empty());
  EXPECT_TRUE(solver.memoryOut(store, 500).empty());
  EXPECT_EQ(solver.memoryOut(store, 200), FlowSensitivePTA::PointsToSet{100});
}

TEST_F(ParallelFlowSensitivePTATest, IndirectCallBoundIsAppliedInBothPolicies) {
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

  for (bool unordered : {false, true}) {
    constexpr uint32_t targetObject = 300;
    constexpr uint32_t otherObject = 301;
    constexpr uint32_t functionPointerId = 400;
    SVFG graph;
    auto *targetAddress = new AddrSVFGNode(1, nullptr, target, targetObject);
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
    config.parallel = true;
    config.workerThreads = 2;
    config.unorderedParallel = unordered;
    config.connectIndirectCall = [&](const CallBase *site,
                                     const Function *callee) {
      return graph.markConnectedCallee(site, callee);
    };
    config.isIndirectCallTargetAllowed = [&](const CallBase *,
                                             const Function *callee) {
      return callee == target;
    };
    FlowSensitivePTA solver(graph, config);
    const auto &stats = solver.solve();
    EXPECT_EQ(stats.indirectCallEdges, 1u) << "unordered=" << unordered;
    EXPECT_GE(stats.indirectCallsRejected, 1u) << "unordered=" << unordered;
    const auto &connected = graph.getConnectedCallees(call);
    EXPECT_EQ(connected.count(target), 1u) << "unordered=" << unordered;
    EXPECT_EQ(connected.count(other), 0u) << "unordered=" << unordered;
  }
}
