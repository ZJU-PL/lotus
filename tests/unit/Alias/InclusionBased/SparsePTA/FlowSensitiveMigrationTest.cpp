#include "Alias/InclusionBased/SparsePTA/Sparse/FlowSensitivePTA.h"
#include "Alias/InclusionBased/SparsePTA/Support/BitvectorClusterer.h"
#include "Alias/InclusionBased/SparsePTA/Versioned/VersionedFlowSensitivePTA.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <vector>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::alias;
using namespace lotus::unittest;

namespace {
TEST_F(LlvmModuleTest, OutputAllocationContractSeparatesSizeAndDestination) {
  AliasSpecManager specs(std::vector<std::string>{});
  lotus::FunctionSpec contract;
  contract.functionName = "custom_output_allocator";
  contract.isAllocator = true;
  contract.allocs.push_back(lotus::AllocEffect{2, 0});
  specs.addCustomSpec(contract.functionName, contract);
  const auto info = specs.getAllocatorInfo(contract.functionName);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->sizeArgIndex, 2);
  EXPECT_EQ(info->ptrOutArgIndex, 0);
  EXPECT_FALSE(info->returnsPointer);
}

class FlowSensitiveMigrationTest : public LlvmModuleTest,
                                   public testing::WithParamInterface<bool> {
protected:
  void build(const char *ir) {
    module = parseModule(ir);
    ASSERT_NE(module, nullptr);
    ICFGBuilder(&icfg).build(module.get());
    SVFGBuilderConfig config;
    config.resolveIndirectCalls = false;
    config.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;
    builder = std::make_unique<SVFGBuilder>(config);
    graph.reset(builder->build(&icfg));
    ASSERT_NE(graph, nullptr);
  }

  void solve() {
    auto connect = [&](const CallBase *call, const Function *callee) {
      std::vector<SVFGEdge *> edges;
      return builder->connectCallSiteToCalleeOnTheFly(graph.get(), call, callee,
                                                      edges);
    };
    auto allowed = [&](const CallBase *call, const Function *callee) {
      const auto targets = builder->getIndirectCallTargets(call);
      return std::find(targets.begin(), targets.end(), callee) != targets.end();
    };
    if (GetParam()) {
      VersionedFlowSensitivePTA::Config config;
      config.connectIndirectCall = connect;
      config.isIndirectCallTargetAllowed = allowed;
      config.preAnalysisTargets = [&](const Value *value) {
        return builder->getPreAnalysisPointsTo(value);
      };
      versioned = std::make_unique<VersionedFlowSensitivePTA>(*graph, config);
      versioned->solve();
    } else {
      FlowSensitivePTA::Config config;
      config.connectIndirectCall = connect;
      config.isIndirectCallTargetAllowed = allowed;
      sparse = std::make_unique<FlowSensitivePTA>(*graph, config);
      sparse->solve();
    }
  }

  const Value *value(StringRef name) {
    return findInstructionByName(module->getFunction("main"), name);
  }
  SVFGNodeBS pointsTo(const Value *value) {
    const auto result =
        versioned ? versioned->pointsTo(value) : sparse->pointsTo(value);
    EXPECT_TRUE(result.has_value());
    return result.value_or(SVFGNodeBS{});
  }
  std::optional<bool> mayAlias(const Value *left, const Value *right) {
    return versioned ? versioned->mayAlias(left, right)
                     : sparse->mayAlias(left, right);
  }
  void expectOnly(StringRef name, const Value *target) {
    const auto result = pointsTo(value(name));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(graph->getObjectValue(*result.begin()), target);
  }

  std::unique_ptr<Module> module;
  ICFG icfg;
  std::unique_ptr<SVFGBuilder> builder;
  std::unique_ptr<SVFG> graph;
  std::unique_ptr<FlowSensitivePTA> sparse;
  std::unique_ptr<VersionedFlowSensitivePTA> versioned;
};

TEST_P(FlowSensitiveMigrationTest, FieldInsensitiveAliasExpandsFields) {
  build(R"(
    define void @main(i64 %n) {
      %a = alloca {i8, i8}
      %base = bitcast {i8, i8}* %a to i8*
      %field = getelementptr {i8, i8}, {i8, i8}* %a, i32 0, i32 1
      %var = getelementptr i8, i8* %base, i64 %n
      ret void
    }
  )");
  solve();
  EXPECT_EQ(mayAlias(value("var"), value("field")), true);
  EXPECT_EQ(mayAlias(value("field"), value("var")), true);
}

TEST_P(FlowSensitiveMigrationTest, BaseAliasExpansionKeepsPreciseMemory) {
  build(R"(
    @x = global i8 0
    @y = global i8 0
    define void @main() {
      %record = alloca {i8*, i8*}
      %first = getelementptr {i8*, i8*}, {i8*, i8*}* %record, i32 0, i32 0
      %second = getelementptr {i8*, i8*}, {i8*, i8*}* %record, i32 0, i32 1
      store i8* @x, i8** %first
      store i8* @y, i8** %second
      %p = load i8*, i8** %first
      %q = load i8*, i8** %second
      ret void
    }
  )");
  solve();
  EXPECT_EQ(mayAlias(value("record"), value("second")), true);
  EXPECT_EQ(mayAlias(value("second"), value("record")), true);
  EXPECT_EQ(mayAlias(value("first"), value("second")), false);
  expectOnly("p", module->getGlobalVariable("x"));
  expectOnly("q", module->getGlobalVariable("y"));
  EXPECT_EQ(mayAlias(value("p"), value("q")), false);
}

TEST_P(FlowSensitiveMigrationTest, LateFieldCollapseInvalidatesEarlierFacts) {
  module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define void @main(i64 %n) {
      %record = alloca {i8*, i8*}
      %first = getelementptr {i8*, i8*}, {i8*, i8*}* %record, i32 0, i32 0
      %second = getelementptr {i8*, i8*}, {i8*, i8*}* %record, i32 0, i32 1
      store i8* @x, i8** %first
      store i8* @y, i8** %second
      %r = load i8*, i8** %first
      %late = getelementptr {i8*, i8*}, {i8*, i8*}* %record, i64 %n
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  graph = std::make_unique<SVFG>();
  auto add = [&](SVFGNode *node, const Value *value) {
    graph->addNode(node);
    if (value)
      graph->setValueNode(value, node->getId());
  };
  constexpr uint32_t base = 500, member = 501, x = 600, y = 601;
  SVFG::ObjectInfo info;
  info.isStack = true;
  info.baseObjId = base;
  graph->setObjectInfo(base, info);
  info.baseObjId = base;
  info.hasFieldOffset = true;
  info.fieldOffset = 8;
  graph->setObjectInfo(member, info);
  graph->setOffsetObject(base, 8, member);
  graph->setObjectBase(member, base);
  graph->setObjectValue(base, value("record"));
  for (auto entry : {std::make_pair(x, "x"), std::make_pair(y, "y")}) {
    SVFG::ObjectInfo global;
    global.isGlobal = true;
    global.baseObjId = entry.first;
    graph->setObjectInfo(entry.first, global);
    graph->setObjectValue(entry.first, module->getGlobalVariable(entry.second));
  }
  auto *address = new AddrSVFGNode(1, nullptr, value("record"), base);
  auto *xAddress = new AddrSVFGNode(2, nullptr, module->getGlobalVariable("x"), x);
  auto *yAddress = new AddrSVFGNode(3, nullptr, module->getGlobalVariable("y"), y);
  auto *first = new GepSVFGNode(4, nullptr, value("first"));
  auto *second = new GepSVFGNode(5, nullptr, value("second"));
  const auto *firstInst = cast<Instruction>(value("first"));
  const auto *storeX = cast<StoreInst>(firstInst->getNextNode()->getNextNode());
  auto *writeX = new StoreSVFGNode(6, nullptr, storeX, first->getId());
  auto *writeY = new StoreSVFGNode(7, nullptr, storeX->getNextNode(), second->getId());
  auto *read = new LoadSVFGNode(8, nullptr, value("r"), first->getId());
  auto *late = new GepSVFGNode(9, nullptr, value("late"));
  add(address, value("record"));
  add(xAddress, module->getGlobalVariable("x"));
  add(yAddress, module->getGlobalVariable("y"));
  add(first, value("first"));
  add(second, value("second"));
  add(writeX, nullptr);
  add(writeY, nullptr);
  add(read, value("r"));
  add(late, value("late"));
  graph->setGepAccess(cast<GetElementPtrInst>(value("first")), 0, false);
  graph->setGepAccess(cast<GetElementPtrInst>(value("second")), 8, false);
  const SVFGNodeBS region{base, member};
  writeX->setMemoryDef(1, 1, region);
  writeY->setMemoryDef(1, 2, region);
  read->setMemoryUse(1, 2, region);
  graph->addEdge(address, first, SVFGEdgeK::IntraGep);
  graph->addEdge(address, second, SVFGEdgeK::IntraGep);
  graph->addEdge(address, late, SVFGEdgeK::IntraGep);
  graph->addEdge(first, writeX, SVFGEdgeK::IntraDirect);
  graph->addEdge(second, writeY, SVFGEdgeK::IntraDirect);
  graph->addEdge(xAddress, writeX, SVFGEdgeK::IntraCopy);
  graph->addEdge(yAddress, writeY, SVFGEdgeK::IntraCopy);
  graph->addEdge(writeX, writeY, SVFGEdgeK::IntraIndirect, nullptr, region);
  graph->addEdge(writeY, read, SVFGEdgeK::IntraIndirect, nullptr, region);
  graph->addEdge(first, read, SVFGEdgeK::IntraDirect);
  // Force discovery of the unannotated GEP after the earlier memory read.
  graph->addEdge(read, late, SVFGEdgeK::IntraIndirect, nullptr, region);
  if (GetParam()) {
    versioned = std::make_unique<VersionedFlowSensitivePTA>(*graph);
    versioned->solve();
  } else {
    sparse = std::make_unique<FlowSensitivePTA>(*graph);
    sparse->solve();
    std::string difference;
    EXPECT_TRUE(sparse->isFixedPoint(&difference)) << difference;
  }
  EXPECT_EQ(pointsTo(value("r")), (SVFGNodeBS{x, y}))
      << (versioned ? versioned->statisticsReport() : sparse->statisticsReport());
}

TEST_P(FlowSensitiveMigrationTest, EmptyAuxiliaryTargetsRemainBounded) {
  build(R"(
    define void @target() { ret void }
    define void @main() {
      call void null()
      ret void
    }
  )");
  const auto *call = dyn_cast<CallBase>(
      &module->getFunction("main")->getEntryBlock().front());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(builder->isIndirectCallTargetAllowed(
                call, module->getFunction("target")),
            false);
  SVFGBuilderConfig withoutPTA;
  withoutPTA.usePointerAnalysis = false;
  SVFGBuilder other(withoutPTA);
  std::unique_ptr<SVFG> otherGraph(other.build(&icfg));
  EXPECT_EQ(other.isIndirectCallTargetAllowed(
                call, module->getFunction("target")),
            std::nullopt);
}

TEST_P(FlowSensitiveMigrationTest, GepFallbackPreservesEveryInputBase) {
  module = parseModule(R"(
    define void @main(i1 %c) {
      %a = alloca {i8*, i8*}
      %b = alloca {i8*, i8*}
      %s = select i1 %c, {i8*, i8*}* %a, {i8*, i8*}* %b
      %p = getelementptr {i8*, i8*}, {i8*, i8*}* %s, i32 0, i32 1
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  graph = std::make_unique<SVFG>();
  auto *a = new AddrSVFGNode(1, nullptr, value("a"), 500);
  auto *b = new AddrSVFGNode(2, nullptr, value("b"), 600);
  auto *selected = new IntraPhiSVFGNode(3, nullptr, value("s"));
  auto *gep = new GepSVFGNode(4, nullptr, value("p"));
  for (SVFGNode *node : std::vector<SVFGNode *>{a, b, selected, gep}) {
    graph->addNode(node);
    graph->setValueNode(node->getValue(), node->getId());
  }
  for (uint32_t base : {500u, 600u}) {
    SVFG::ObjectInfo info;
    info.isStack = true;
    info.baseObjId = base;
    graph->setObjectInfo(base, info);
    info.fieldOffset = 8;
    info.hasFieldOffset = true;
    graph->setObjectInfo(base + 1, info);
  }
  graph->setOffsetObject(500, 8, 501);
  graph->setObjectsForValue(value("p"), SVFGNodeBS{501, 601});
  graph->setGepAccess(cast<GetElementPtrInst>(value("p")), 8, false);
  graph->addEdge(a, selected, SVFGEdgeK::IntraPhi);
  graph->addEdge(b, selected, SVFGEdgeK::IntraPhi);
  graph->addEdge(selected, gep, SVFGEdgeK::IntraGep);
  if (GetParam()) {
    versioned = std::make_unique<VersionedFlowSensitivePTA>(*graph);
    versioned->solve();
  } else {
    sparse = std::make_unique<FlowSensitivePTA>(*graph);
    sparse->solve();
  }
  EXPECT_EQ(pointsTo(value("p")), (SVFGNodeBS{501, 601}));
}

TEST_P(FlowSensitiveMigrationTest, MissingStorePrelabelDoesNotWriteIntoThePast) {
  build(R"(
    @old = global i8 0
    @new = global i8 0
    @slot = global i8* @old
    @other = global i8* null
    define void @main() {
      %before = load i8*, i8** @slot
      %lhs = bitcast i8** @slot to i8**
      store i8* @new, i8** %lhs
      %after = load i8*, i8** @slot
      %unused = load i8*, i8** @other
      ret void
    }
  )");
  if (GetParam()) {
    VersionedFlowSensitivePTA::Config config;
    config.preAnalysisTargets = [&](const Value *pointer) {
      if (pointer == value("lhs"))
        return SVFGNodeBS{graph->getObjectId(module->getGlobalVariable("other"))};
      return builder->getPreAnalysisPointsTo(pointer);
    };
    versioned = std::make_unique<VersionedFlowSensitivePTA>(*graph, config);
    versioned->solve();
  } else {
    sparse = std::make_unique<FlowSensitivePTA>(*graph);
    sparse->solve();
  }
  expectOnly("before", module->getGlobalVariable("old"));
  expectOnly("after", module->getGlobalVariable("new"));
}

TEST_P(FlowSensitiveMigrationTest, RegisterAggregateSelectPreservesPointers) {
  build(R"(
    @x = global i8 0
    @y = global i8 0
    define void @main(i1 %c) {
      %a = insertvalue {i8*, i8*} undef, i8* @x, 0
      %b = insertvalue {i8*, i8*} undef, i8* @y, 0
      %s = select i1 %c, {i8*, i8*} %a, {i8*, i8*} %b
      %p = extractvalue {i8*, i8*} %s, 0
      ret void
    }
  )");
  solve();
  const auto result = pointsTo(value("p"));
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(mayAlias(value("p"), module->getGlobalVariable("x")), true);
  EXPECT_EQ(mayAlias(value("p"), module->getGlobalVariable("y")), true);
}

TEST_P(FlowSensitiveMigrationTest, NestedAggregateOverwriteAndCyclicPhi) {
  build(R"(
    @x = global i8 0
    @y = global i8 0
    define void @main(i1 %c) {
    entry:
      %a = insertvalue {{i8*, i8*}} zeroinitializer, i8* @x, 0, 0
      br label %loop
    loop:
      %s = phi {{i8*, i8*}} [%a, %entry], [%next, %loop]
      %p = extractvalue {{i8*, i8*}} %s, 0, 0
      %b = insertvalue {{i8*, i8*}} %s, i8* @y, 0, 0
      %next = insertvalue {{i8*, i8*}} %b, i8* @x, 0, 1
      %overwritten = extractvalue {{i8*, i8*}} %b, 0, 0
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    }
  )");
  solve();
  EXPECT_EQ(pointsTo(value("p")).size(), 2u);
  expectOnly("overwritten", module->getGlobalVariable("y"));
}

TEST_P(FlowSensitiveMigrationTest, AggregateFieldsCrossDirectCalls) {
  build(R"(
    @x = global i8 0
    define {i8*} @identity({i8*} %arg) {
      ret {i8*} %arg
    }
    define void @main() {
      %a = insertvalue {i8*} undef, i8* @x, 0
      %b = call {i8*} @identity({i8*} %a)
      %p = extractvalue {i8*} %b, 0
      ret void
    }
  )");
  solve();
  expectOnly("p", module->getGlobalVariable("x"));
}

TEST_P(FlowSensitiveMigrationTest, ExtractedAggregatePointerOrdersMemory) {
  build(R"(
    @x = global i8 0
    @y = global i8 0
    define void @main() {
      %slot = alloca i8*
      store i8* @x, i8** %slot
      %a = insertvalue {i8**} undef, i8** %slot, 0
      %p = extractvalue {i8**} %a, 0
      store i8* @y, i8** %p
      %r = load i8*, i8** %slot
      ret void
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("y"));
}

TEST_P(FlowSensitiveMigrationTest, UnsupportedAggregateSourceIsUnknown) {
  build(R"(
    @x = global i8 0
    declare {i8*} @external()
    define void @main() {
      %xptr = bitcast i8* @x to i8*
      %a = call {i8*} @external()
      %p = extractvalue {i8*} %a, 0
      ret void
    }
  )");
  solve();
  const auto result = pointsTo(value("p"));
  ASSERT_EQ(result.size(), 1u);
  EXPECT_TRUE(graph->isUnknownObject(*result.begin()));
  EXPECT_EQ(mayAlias(value("p"), module->getGlobalVariable("x")), true);
}

TEST_P(FlowSensitiveMigrationTest, ExternalAllocationWritesThroughArgument) {
  build(R"(
    declare i32 @posix_memalign(i8**, i64, i64)
    define void @main() {
      %slot = alloca i8*
      store i8* null, i8** %slot
      %rc = call i32 @posix_memalign(i8** %slot, i64 8, i64 8)
      %p = load i8*, i8** %slot
      %q = load i8*, i8** %slot
      ret void
    }
  )");
  solve();
  const auto result = pointsTo(value("p"));
  ASSERT_EQ(result.size(), 1u);
  EXPECT_TRUE(graph->getObjectInfo(*result.begin())->isHeap);
  EXPECT_EQ(pointsTo(value("q")), result);
  EXPECT_EQ(mayAlias(value("p"), value("q")), true);
}

TEST_P(FlowSensitiveMigrationTest, ExternalAllocationPreservesFailureValue) {
  build(R"(
    @old = global i8 0
    declare i32 @posix_memalign(i8**, i64, i64)
    define void @allocate(i8** %out) {
      %rc = call i32 @posix_memalign(i8** %out, i64 8, i64 8)
      ret void
    }
    define void @main() {
      %slot = alloca i8*
      store i8* @old, i8** %slot
      call void @allocate(i8** %slot)
      %p = load i8*, i8** %slot
      ret void
    }
  )");
  solve();
  EXPECT_EQ(pointsTo(value("p")).size(), 2u);
  EXPECT_EQ(mayAlias(value("p"), module->getGlobalVariable("old")), true);
}

TEST_P(FlowSensitiveMigrationTest, IndirectExternalAllocationWritesMemory) {
  build(R"(
    @fp = global i32 (i8**, i64, i64)* @posix_memalign
    declare i32 @posix_memalign(i8**, i64, i64)
    define void @main() {
      %slot = alloca i8*
      store i8* null, i8** %slot
      %f = load i32 (i8**, i64, i64)*, i32 (i8**, i64, i64)** @fp
      %rc = call i32 %f(i8** %slot, i64 8, i64 8)
      %p = load i8*, i8** %slot
      ret void
    }
  )");
  solve();
  const auto result = pointsTo(value("p"));
  ASSERT_EQ(result.size(), 1u);
  EXPECT_TRUE(graph->getObjectInfo(*result.begin())->isHeap);
}

TEST_P(FlowSensitiveMigrationTest, UnknownPointeeMayAliasConcreteObject) {
  build(R"(
    @x = global i8 0
    @slot = global i8* null
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)
    define void @main() {
      %s = bitcast i8** @slot to i8*
      call void @llvm.memset.p0i8.i64(i8* %s, i8 1, i64 8, i1 false)
      %r = load i8*, i8** @slot
      %xptr = bitcast i8* @x to i8*
      ret void
    }
  )");
  solve();
  const auto result = pointsTo(value("r"));
  ASSERT_EQ(result.size(), 1u);
  ASSERT_TRUE(graph->isUnknownObject(*result.begin()));
  EXPECT_EQ(mayAlias(value("r"), value("xptr")), true);
  EXPECT_EQ(mayAlias(value("xptr"), value("r")), true);
}

TEST_P(FlowSensitiveMigrationTest, ConstantGepOperandSurvivesStoreAndPhi) {
  build(R"(
    @arr = global {i8, i8} zeroinitializer
    @slot = global i8* null
    define i8* @main(i1 %c) {
    entry:
      store i8* getelementptr ({i8, i8}, {i8, i8}* @arr, i32 0, i32 1), i8** @slot
      br i1 %c, label %left, label %right
    left:
      br label %join
    right:
      br label %join
    join:
      %p = phi i8* [getelementptr ({i8, i8}, {i8, i8}* @arr, i32 0, i32 1), %left], [null, %right]
      %r = load i8*, i8** @slot
      ret i8* %r
    }
  )");
  solve();
  const auto result = pointsTo(value("r"));
  ASSERT_EQ(result.size(), 1u);
  const auto *info = graph->getObjectInfo(*result.begin());
  ASSERT_NE(info, nullptr);
  EXPECT_TRUE(info->hasFieldOffset);
  EXPECT_EQ(info->fieldOffset, 1u);
  EXPECT_EQ(pointsTo(value("p")), result);
}

TEST_P(FlowSensitiveMigrationTest, ArrayOfStructKeepsMemberOffsets) {
  build(R"(
    @x = global i8 0
    @y = global i8 0
    @arr = global [2 x {i8*, i8*}] [{i8*, i8*} {i8* @x, i8* @y}, {i8*, i8*} {i8* @x, i8* @y}]
    define i8* @main(i64 %n) {
      %p = getelementptr [2 x {i8*, i8*}], [2 x {i8*, i8*}]* @arr, i32 0, i64 %n, i32 1
      %r = load i8*, i8** %p
      %cp = getelementptr [2 x {i8*, i8*}], [2 x {i8*, i8*}]* @arr, i32 0, i32 1, i32 1
      %constant = load i8*, i8** %cp
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("y"));
  expectOnly("constant", module->getGlobalVariable("y"));
}

TEST_P(FlowSensitiveMigrationTest,
       ArrayMemberStoreDoesNotOverwriteOtherMember) {
  build(R"(
    @x = global i8 0
    @y = global i8 0
    @arr = global [2 x {i8*, i8*}] [{i8*, i8*} {i8* @x, i8* @y}, {i8*, i8*} {i8* @x, i8* @y}]
    define i8* @main() {
      %p = getelementptr [2 x {i8*, i8*}], [2 x {i8*, i8*}]* @arr, i32 0, i32 0, i32 1
      store i8* @x, i8** %p
      %other = getelementptr [2 x {i8*, i8*}], [2 x {i8*, i8*}]* @arr, i32 0, i32 1, i32 0
      %r = load i8*, i8** %other
      %updated = load i8*, i8** %p
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("x"));
  const auto updated = pointsTo(value("updated"));
  EXPECT_EQ(updated.size(), 2u); // Array members still require weak updates.
}

TEST_P(FlowSensitiveMigrationTest, BottomGepDoesNotImportPreAnalysisTargets) {
  build(R"(
    @obj = global {i8, i8} zeroinitializer
    @slot = global {i8, i8}* null
    define void @main(i1 %c) {
      br i1 %c, label %init, label %dead
    init:
      store {i8, i8}* @obj, {i8, i8}** @slot
      ret void
    dead:
      %p = load {i8, i8}*, {i8, i8}** @slot
      %field = getelementptr {i8, i8}, {i8, i8}* %p, i32 0, i32 1
      ret void
    }
  )");
  solve();
  EXPECT_TRUE(pointsTo(value("p")).empty());
  EXPECT_TRUE(pointsTo(value("field")).empty());
}

TEST_P(FlowSensitiveMigrationTest, InitializerReachesSuccessiveMeldJoins) {
  build(R"(
    @x = global i8 0
    @slot = global i8* @x
    define i8* @main(i1 %a, i1 %b) {
      br i1 %a, label %clear1, label %join1
    clear1:
      store i8* null, i8** @slot
      br label %join1
    join1:
      br i1 %b, label %clear2, label %join2
    clear2:
      store i8* null, i8** @slot
      br label %join2
    join2:
      %r = load i8*, i8** @slot
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("x"));
}

TEST_P(FlowSensitiveMigrationTest, ExternalReturnUsesArgumentSummary) {
  build(R"(
    @x = global i8 0
    declare i8* @strchr(i8*, i32)
    define i8* @main() {
      %r = call i8* @strchr(i8* @x, i32 0)
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("x"));
}

TEST_P(FlowSensitiveMigrationTest, IndirectExternalReturnUsesArgumentSummary) {
  build(R"(
    @x = global i8 0
    @fp = global i8* (i8*, i32)* @strchr
    declare i8* @strchr(i8*, i32)
    define i8* @main() {
      %f = load i8* (i8*, i32)*, i8* (i8*, i32)** @fp
      %r = call i8* %f(i8* @x, i32 0)
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("x"));
}

TEST_P(FlowSensitiveMigrationTest,
       IndirectUnknownExternalReturnIsConservative) {
  build(R"(
    @x = global i8 0
    @fp = global i8* ()* @opaque
    declare i8* @opaque()
    define i8* @main() {
      %xptr = bitcast i8* @x to i8*
      %f = load i8* ()*, i8* ()** @fp
      %r = call i8* %f()
      ret i8* %r
    }
  )");
  solve();
  EXPECT_EQ(mayAlias(value("r"), value("xptr")), true);
}

TEST_P(FlowSensitiveMigrationTest, UnknownExternalReturnIsConservative) {
  build(R"(
    @x = global i8 0
    declare i8* @opaque()
    define i8* @main() {
      %xptr = bitcast i8* @x to i8*
      %r = call i8* @opaque()
      ret i8* %r
    }
  )");
  solve();
  EXPECT_EQ(mayAlias(value("r"), value("xptr")), true);
}

TEST_P(FlowSensitiveMigrationTest, ReallocatorKeepsOldAliasAndFreshAllocation) {
  build(R"(
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)
    define i8* @main() {
      %p = call i8* @malloc(i64 8)
      %r = call i8* @realloc(i8* %p, i64 16)
      ret i8* %r
    }
  )");
  solve();
  EXPECT_EQ(mayAlias(value("p"), value("r")), true);
  const auto result = pointsTo(value("r"));
  EXPECT_EQ(result.size(), 2u);
  for (uint32_t object : result)
    EXPECT_TRUE(graph->isHeapObject(object));
}

TEST_P(FlowSensitiveMigrationTest,
       StrongFunctionPointerUpdateExcludesOldCallee) {
  build(R"(
    @x = global i8 0
    @y = global i8 0
    @fp = global i8* (i8*)* @old
    define i8* @old(i8* %a) { ret i8* @y }
    define i8* @new(i8* %a) { ret i8* %a }
    define i8* @main() {
      store i8* (i8*)* @new, i8* (i8*)** @fp
      %f = load i8* (i8*)*, i8* (i8*)** @fp
      %r = call i8* %f(i8* @x)
      ret i8* %r
    }
  )");
  solve();
  expectOnly("f", module->getFunction("new"));
  expectOnly("r", module->getGlobalVariable("x"));
  const auto *call = cast<CallBase>(value("r"));
  EXPECT_FALSE(graph->hasConnectedCallee(call, module->getFunction("old")));
  EXPECT_TRUE(graph->hasConnectedCallee(call, module->getFunction("new")));
}

TEST_P(FlowSensitiveMigrationTest, SelectedStructStoreReachesBothMembers) {
  build(R"(
    @a = global i8 0
    @b = global i8 0
    @record = global {i8*, i8*} {i8* @a, i8* @b}
    define i8* @main(i1 %condition) {
      %first = getelementptr {i8*, i8*}, {i8*, i8*}* @record, i32 0, i32 0
      %second = getelementptr {i8*, i8*}, {i8*, i8*}* @record, i32 0, i32 1
      %destination = select i1 %condition, i8** %first, i8** %second
      store i8* @a, i8** %destination
      %r = load i8*, i8** %second
      ret i8* %r
    }
  )");
  solve();
  const auto result = pointsTo(value("r"));
  std::set<const Value *> targets;
  for (uint32_t object : result)
    targets.insert(graph->getObjectValue(object));
  EXPECT_EQ(targets, (std::set<const Value *>{module->getGlobalVariable("a"),
                                              module->getGlobalVariable("b")}));
}

TEST_P(FlowSensitiveMigrationTest, IndirectNoopPreservesGlobalMemory) {
  build(R"(
    @old = global i8 0
    @new = global i8 0
    @slot = global i8* @old
    @fp = global void ()* @write
    define void @write() {
      store i8* @new, i8** @slot
      ret void
    }
    define void @noop() { ret void }
    define i8* @main() {
      store void ()* @noop, void ()** @fp
      %f = load void ()*, void ()** @fp
      call void %f()
      %r = load i8*, i8** @slot
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("old"));
}

TEST_P(FlowSensitiveMigrationTest, IndirectWriterDoesNotBypassStrongUpdate) {
  build(R"(
    @old = global i8 0
    @new = global i8 0
    @slot = global i8* @old
    @fp = global void ()* @noop
    define void @write() {
      store i8* @new, i8** @slot
      ret void
    }
    define void @noop() { ret void }
    define i8* @main() {
      store void ()* @write, void ()** @fp
      %f = load void ()*, void ()** @fp
      call void %f()
      %r = load i8*, i8** @slot
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("new"));
}

TEST_P(FlowSensitiveMigrationTest,
       IndirectAlternativesPreserveUntouchedObjects) {
  build(R"(
    @a = global i8 0
    @b = global i8 0
    @c = global i8 0
    @d = global i8 0
    @g0 = global i8* @a
    @g1 = global i8* @c
    @fp = global void ()* @left
    define void @left() {
      store i8* @b, i8** @g0
      ret void
    }
    define void @right() {
      store i8* @d, i8** @g1
      ret void
    }
    define i8* @main(i1 %condition) {
      %target = select i1 %condition, void ()* @left, void ()* @right
      store void ()* %target, void ()** @fp
      %f = load void ()*, void ()** @fp
      call void %f()
      %r0 = load i8*, i8** @g0
      %r1 = load i8*, i8** @g1
      ret i8* %r0
    }
  )");
  solve();
  auto targetsOf = [&](StringRef name) {
    std::set<const Value *> targets;
    for (uint32_t object : pointsTo(value(name)))
      targets.insert(graph->getObjectValue(object));
    return targets;
  };
  EXPECT_EQ(targetsOf("r0"),
            (std::set<const Value *>{module->getGlobalVariable("a"),
                                     module->getGlobalVariable("b")}));
  EXPECT_EQ(targetsOf("r1"),
            (std::set<const Value *>{module->getGlobalVariable("c"),
                                     module->getGlobalVariable("d")}));
}

TEST_P(FlowSensitiveMigrationTest, MemcpyInitializesPreviouslyUnstoredLocal) {
  build(R"(
    @x = global i8 0
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    define i8* @main() {
      %source = alloca i8*
      %destination = alloca i8*
      store i8* @x, i8** %source
      %src = bitcast i8** %source to i8*
      %dst = bitcast i8** %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst, i8* %src, i64 8, i1 false)
      %r = load i8*, i8** %destination
      ret i8* %r
    }
  )");
  solve();
  expectOnly("r", module->getGlobalVariable("x"));
}

INSTANTIATE_TEST_SUITE_P(SparseAndVersioned, FlowSensitiveMigrationTest,
                         testing::Bool());

TEST(FlowSensitiveMigrationSupportTest, ClusteringScalesWithObservedSparseIds) {
  const std::vector<SVFGNodeBS> sets = {{1u << 30, (1u << 30) + 3},
                                        {UINT32_MAX - 1}};
  flow_sensitive::BitvectorClusterer clusterer(sets);
  for (const auto &set : sets)
    EXPECT_EQ(clusterer.toOriginal(clusterer.toClustered(set)), set);
  // The sparse permutation must remain a bijection even for unobserved IDs
  // which would otherwise collide with dense destinations 1, 2 and 3.
  std::set<uint32_t> clustered;
  const std::vector<uint32_t> objects = {
      0, 1, 2, 3, 4, 17, 1u << 30, (1u << 30) + 3, UINT32_MAX - 1};
  for (uint32_t object : objects) {
    const uint32_t mapped = clusterer.toClustered(object);
    EXPECT_TRUE(clustered.insert(mapped).second);
    EXPECT_EQ(clusterer.toOriginal(mapped), object);
  }
  EXPECT_LE(clusterer.toClustered(UINT32_MAX - 1), 3u);
}

TEST_F(LlvmModuleTest, VersionPrelabelsKeepEveryAuxiliaryTarget) {
  auto module = parseModule(R"(
    define void @main(i8** %p) {
      store i8* null, i8** %p
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  const auto *instruction =
      &module->getFunction("main")->getEntryBlock().front();
  SVFG graph;
  auto *store = new StoreSVFGNode(1, nullptr, instruction, 0);
  store->setMemoryDef(1, 1, {100});
  graph.addNode(store);
  SVFG::ObjectInfo info;
  info.isGlobal = true;
  graph.setObjectInfo(100, info);
  graph.setObjectInfo(101, info);
  VersionedFlowSensitivePTA::Config config;
  config.preAnalysisTargets = [](const Value *) {
    return SVFGNodeBS{100, 101};
  };
  VersionedFlowSensitivePTA solver(graph, config);
  solver.solve();
  EXPECT_NE(solver.getYield(1, 100), VersionedFlowSensitivePTA::InvalidVersion);
  EXPECT_NE(solver.getYield(1, 101), VersionedFlowSensitivePTA::InvalidVersion);
}

TEST_F(LlvmModuleTest, AliasQueryExpandsDistinctFieldInsensitiveIdentity) {
  auto module = parseModule(R"(
    @obj = global {i8, i8} zeroinitializer
    define void @main() {
      %summary = bitcast {i8, i8}* @obj to i8*
      %field = getelementptr {i8, i8}, {i8, i8}* @obj, i32 0, i32 1
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  const Value *summary =
      findInstructionByName(module->getFunction("main"), "summary");
  const Value *field =
      findInstructionByName(module->getFunction("main"), "field");
  SVFG graph;
  graph.addNode(new AddrSVFGNode(1, nullptr, summary, 102));
  graph.addNode(new AddrSVFGNode(2, nullptr, field, 101));
  graph.setValueNode(summary, 1);
  graph.setValueNode(field, 2);
  SVFG::ObjectInfo member;
  member.baseObjId = 100;
  member.hasFieldOffset = true;
  member.fieldOffset = 1;
  graph.setObjectInfo(101, member);
  SVFG::ObjectInfo insensitive;
  insensitive.baseObjId = 100;
  insensitive.isFieldInsensitive = true;
  graph.setObjectInfo(102, insensitive);
  FlowSensitivePTA sparse(graph);
  sparse.solve();
  EXPECT_NE(sparse.pointsTo(graph.getValueNode(summary)),
            sparse.pointsTo(graph.getValueNode(field)));
  EXPECT_EQ(sparse.mayAlias(summary, field), true);
  VersionedFlowSensitivePTA versioned(graph);
  versioned.solve();
  EXPECT_EQ(versioned.mayAlias(summary, field), true);
}
} // namespace
