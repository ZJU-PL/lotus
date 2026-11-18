#include "IR/SVFG/SVFGOPT.h"
#include "IR/SVFG/SVFGNode.h"
#include "SVFGMemorySSATestSupport.h"

#include <optional>

#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;
using namespace lotus::analysis;

namespace {

class SVFGModRefTest : public SVFGMemorySSATest {
protected:
  std::unique_ptr<SVFG> buildWithPTA(Module *module, ICFG &icfg) {
    ICFGBuilder icfgBuilder(&icfg);
    icfgBuilder.build(module);

    SVFGBuilderConfig cfg;
    cfg.usePointerAnalysis = true;
    cfg.buildMSSA = true;
    cfg.pointerOnly = false;

    SVFGBuilder builder(cfg);
    return std::unique_ptr<SVFG>(builder.build(&icfg));
  }

  static const CallBase *findCall(const Function *function, StringRef name) {
    for (const BasicBlock &block : *function) {
      for (const Instruction &instruction : block) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        if (!call)
          continue;
        const Function *callee = call->getCalledFunction();
        if (callee && callee->getName() == name)
          return call;
      }
    }
    return nullptr;
  }
};

SVFGNodeBS objectsOf(const SVFG &graph, const Value *value) {
  return graph.getObjectIds(value);
}

} // namespace

TEST_F(SVFGModRefTest, ReadAndWriteEffectsOfCalls) {
  const char *source = R"(
    @g = global i32 0
    @h = global i32 0

    define void @writer(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define void @reader(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret void
    }

    declare void @pure(i8*) readnone
    declare void @unknown_ext(i8*)

    define i32 @main() {
    entry:
      call void @writer(i32* @g)
      call void @reader(i32* @g)
      call void @pure(i8* bitcast (i32* @g to i8*))
      call void @unknown_ext(i8* bitcast (i32* @h to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ICFG icfg;
  auto graph = buildWithPTA(module.get(), icfg);
  ASSERT_NE(graph, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *writer = findCall(mainFn, "writer");
  const auto *reader = findCall(mainFn, "reader");
  const auto *pure = findCall(mainFn, "pure");
  const auto *unknown = findCall(mainFn, "unknown_ext");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(pure, nullptr);
  ASSERT_NE(unknown, nullptr);

  const Value *g = module->getGlobalVariable("g");
  const Value *h = module->getGlobalVariable("h");
  ASSERT_NE(g, nullptr);
  ASSERT_NE(h, nullptr);
  const SVFGNodeBS gObjects = objectsOf(*graph, g);
  ASSERT_FALSE(gObjects.empty());
  const uint32_t gObject = *gObjects.begin();

  // writer writes @g and does not read it.
  const auto writerInfo = graph->getModRefInfo(writer, gObject);
  ASSERT_TRUE(writerInfo.has_value());
  EXPECT_TRUE(writerInfo->writes);
  EXPECT_FALSE(writerInfo->reads);
  EXPECT_EQ(writerInfo->kind(), SVFG::ModRefInfo::Kind::Mod);
  // The pointer overload resolves the pointee objects.
  const auto writerFromPointer = graph->getModRefInfo(writer, g);
  ASSERT_TRUE(writerFromPointer.has_value());
  EXPECT_TRUE(writerFromPointer->writes);
  EXPECT_FALSE(writerFromPointer->reads);

  // reader reads @g and does not write it.
  const auto readerInfo = graph->getModRefInfo(reader, gObject);
  ASSERT_TRUE(readerInfo.has_value());
  EXPECT_TRUE(readerInfo->reads);
  EXPECT_FALSE(readerInfo->writes);
  EXPECT_EQ(readerInfo->kind(), SVFG::ModRefInfo::Kind::Ref);

  // A different global is untouched by both calls.
  const SVFGNodeBS hObjects = objectsOf(*graph, h);
  ASSERT_FALSE(hObjects.empty());
  const auto untouched = graph->getModRefInfo(writer, *hObjects.begin());
  ASSERT_TRUE(untouched.has_value());
  EXPECT_FALSE(untouched->isModRef());

  // Whole-call queries.
  const auto writerWhole = graph->getModRefInfo(writer);
  ASSERT_TRUE(writerWhole.has_value());
  EXPECT_FALSE(writerWhole->reads);
  EXPECT_TRUE(writerWhole->writes);
  const auto readerWhole = graph->getModRefInfo(reader);
  ASSERT_TRUE(readerWhole.has_value());
  EXPECT_TRUE(readerWhole->reads);
  EXPECT_FALSE(readerWhole->writes);

  // Directed call-vs-call conflicts, matching SVF's getModRefInfo(cs1, cs2).
  const auto writerVsReader = graph->getModRefInfo(writer, reader);
  ASSERT_TRUE(writerVsReader.has_value());
  EXPECT_FALSE(writerVsReader->reads);
  EXPECT_TRUE(writerVsReader->writes);
  const auto readerVsWriter = graph->getModRefInfo(reader, writer);
  ASSERT_TRUE(readerVsWriter.has_value());
  EXPECT_TRUE(readerVsWriter->reads);
  EXPECT_FALSE(readerVsWriter->writes);

  // Two calls to the same writer do not read, but their writes conflict.
  const auto writerVsWriter = graph->getModRefInfo(writer, writer);
  ASSERT_TRUE(writerVsWriter.has_value());
  EXPECT_FALSE(writerVsWriter->reads);
  EXPECT_TRUE(writerVsWriter->writes);

  // An unrelated call has no conflict.
  const auto writerVsUnknown = graph->getModRefInfo(writer, unknown);
  ASSERT_TRUE(writerVsUnknown.has_value());
  EXPECT_FALSE(writerVsUnknown->isModRef());

  // Calls without MemorySSA nodes fall back to LLVM memory attributes.
  const auto pureInfo = graph->getModRefInfo(pure, g);
  ASSERT_TRUE(pureInfo.has_value());
  EXPECT_FALSE(pureInfo->isModRef());

  // An unknown external call with pointer arguments is conservatively both a
  // read and a write.
  const auto unknownInfo = graph->getModRefInfo(unknown, h);
  ASSERT_TRUE(unknownInfo.has_value());
  EXPECT_TRUE(unknownInfo->reads);
  EXPECT_TRUE(unknownInfo->writes);
}

TEST_F(SVFGModRefTest, AggregateQueryCoversFields) {
  const char *source = R"(
    %struct = type { i32, i32, i32 }

    define void @field_writer(%struct* %p) {
    entry:
      %field1 = getelementptr %struct, %struct* %p, i64 0, i32 1
      store i32 7, i32* %field1
      ret void
    }

    define i32 @main() {
    entry:
      %s = alloca %struct
      %field1 = getelementptr %struct, %struct* %s, i64 0, i32 1
      %field2 = getelementptr %struct, %struct* %s, i64 0, i32 2
      call void @field_writer(%struct* %s)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ICFG icfg;
  auto graph = buildWithPTA(module.get(), icfg);
  ASSERT_NE(graph, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *call = findCall(mainFn, "field_writer");
  ASSERT_NE(call, nullptr);

  const Value *s = nullptr;
  const Value *field1 = nullptr;
  const Value *field2 = nullptr;
  for (const Instruction &instruction : instructions(mainFn)) {
    if (const auto *alloca = dyn_cast<AllocaInst>(&instruction))
      s = alloca;
    if (const auto *gep = dyn_cast<GetElementPtrInst>(&instruction)) {
      const auto *index = dyn_cast<ConstantInt>(gep->getOperand(2));
      if (!index)
        continue;
      if (index->getZExtValue() == 1)
        field1 = gep;
      else if (index->getZExtValue() == 2)
        field2 = gep;
    }
  }
  ASSERT_NE(s, nullptr);
  ASSERT_NE(field1, nullptr);
  ASSERT_NE(field2, nullptr);

  // The call writes field 1.
  const auto field1Info = graph->getModRefInfo(call, field1);
  ASSERT_TRUE(field1Info.has_value());
  EXPECT_TRUE(field1Info->writes);

  // MemorySSA regions for a field write also include the aggregate base, so a
  // sibling-field query is conservatively reported as modified. The aggregate
  // query must be true, and an unrelated object must stay false (covered by
  // ReadAndWriteEffectsOfCalls).
  const auto field2Info = graph->getModRefInfo(call, field2);
  ASSERT_TRUE(field2Info.has_value());

  // A query for the aggregate is affected by a write to one of its fields.
  const auto aggregateInfo = graph->getModRefInfo(call, s);
  ASSERT_TRUE(aggregateInfo.has_value());
  EXPECT_TRUE(aggregateInfo->writes);
}

TEST_F(SVFGModRefTest, ModRefSurvivesSVFGOPT) {
  const char *source = R"(
    @g = global i32 0

    define void @writer(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      call void @writer(i32* @g)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ICFG icfg;
  auto graph = buildWithPTA(module.get(), icfg);
  ASSERT_NE(graph, nullptr);

  const auto *writer = findCall(module->getFunction("main"), "writer");
  ASSERT_NE(writer, nullptr);

  SVFGOPT optimized;
  ASSERT_TRUE(optimized.adoptAndOptimize(std::move(graph)));
  // The builder-time write summary must move with the graph even when SVFGOPT
  // rewires or removes memory nodes.
  ASSERT_NE(optimized.getCallWriteObjects(writer), nullptr);

  const auto info = optimized.getModRefInfo(writer, module->getGlobalVariable("g"));
  ASSERT_TRUE(info.has_value());
  EXPECT_TRUE(info->writes);
  EXPECT_FALSE(info->reads);
}

TEST_F(SVFGModRefTest, EmptyQueryIsNotAnEffect) {
  const char *source = R"(
    @g = global i32 0

    define void @writer(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      call void @writer(i32* @g)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ICFG icfg;
  auto graph = buildWithPTA(module.get(), icfg);
  ASSERT_NE(graph, nullptr);

  const auto *writer = findCall(module->getFunction("main"), "writer");
  ASSERT_NE(writer, nullptr);

  const auto empty = graph->getModRefInfo(writer, SVFGNodeBS{});
  ASSERT_TRUE(empty.has_value());
  EXPECT_FALSE(empty->isModRef());
  EXPECT_FALSE(graph->getModRefInfo(writer, static_cast<const Value *>(nullptr))
                  .has_value());
  EXPECT_FALSE(graph->getModRefInfo(nullptr, 1).has_value());
}
