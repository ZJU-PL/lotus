#include "Analysis/TypeHierarchy/LLVMVFTable.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include "Analysis/TypeHierarchy/DIBasedTypeHierarchyData.h"
#include "Analysis/TypeHierarchy/LLVMVFTableData.h"
#include "Analysis/TypeHierarchy/TypeHierarchyAnalysis.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus;

namespace {

TEST(LLVMVFTableTest, PrintsNullAndRepeatedSlotsByPosition) {
  LLVMContext Context;
  Module M("vtable-test", Context);
  auto *FTy = FunctionType::get(Type::getVoidTy(Context), false);
  auto *F = Function::Create(FTy, Function::ExternalLinkage, "repeated", M);
  LLVMVFTable VTable({F, nullptr, F});

  std::string Printed;
  raw_string_ostream OS(Printed);
  VTable.print(OS);
  OS.flush();

  EXPECT_EQ(Printed, "repeated\n__null__\nrepeated");
}

TEST(LLVMVFTableDataTest, EscapedNamesRoundTripThroughJson) {
  LLVMVFTableData Original;
  Original.VFT = {"quoted\"function", "path\\function", "line\nfunction"};

  std::string Serialized;
  raw_string_ostream OS(Serialized);
  Original.printAsJson(OS);
  OS.flush();

  auto Parsed = json::parse(Serialized);
  if (!Parsed)
    FAIL() << toString(Parsed.takeError());
  EXPECT_EQ(LLVMVFTableData::loadJsonString(Serialized).VFT, Original.VFT);
}

TEST(DIBasedTypeHierarchyDataTest, EscapedNamesProduceValidJson) {
  DIBasedTypeHierarchyData Data;
  Data.VertexTypes = {"type\"name"};
  Data.TransitiveDerivedIndex = {{0, 1}};
  Data.Hierarchy = {0};
  Data.VTables = {{"line\nfunction"}};

  std::string Serialized;
  raw_string_ostream OS(Serialized);
  Data.printAsJson(OS);
  OS.flush();

  auto Parsed = json::parse(Serialized);
  if (!Parsed)
    FAIL() << toString(Parsed.takeError());
  auto RoundTrip = DIBasedTypeHierarchyData::loadJsonString(Serialized);
  ASSERT_TRUE(static_cast<bool>(RoundTrip))
      << llvm::toString(RoundTrip.takeError());
  EXPECT_EQ(RoundTrip->VertexTypes, Data.VertexTypes);
  EXPECT_EQ(RoundTrip->TransitiveDerivedIndex, Data.TransitiveDerivedIndex);
  EXPECT_EQ(RoundTrip->Hierarchy, Data.Hierarchy);
  EXPECT_EQ(RoundTrip->VTables, Data.VTables);
}

TEST(DIBasedTypeHierarchyDataTest, RejectsMalformedAndOutOfRangeData) {
  auto Negative = DIBasedTypeHierarchyData::loadJsonString(R"({
    "VertexTypes": ["A"],
    "TransitiveDerivedIndex": [[0, 1]],
    "Hierarchy": [-1],
    "VTables": [[]]
  })");
  EXPECT_FALSE(static_cast<bool>(Negative));
  llvm::consumeError(Negative.takeError());

  auto Overflow = DIBasedTypeHierarchyData::loadJsonString(R"({
    "VertexTypes": ["A"],
    "TransitiveDerivedIndex": [[0, 1]],
    "Hierarchy": [4294967296],
    "VTables": [[]]
  })");
  EXPECT_FALSE(static_cast<bool>(Overflow));
  llvm::consumeError(Overflow.takeError());

  auto InvalidRange = DIBasedTypeHierarchyData::loadJsonString(R"({
    "VertexTypes": ["A"],
    "TransitiveDerivedIndex": [[0, 2]],
    "Hierarchy": [0],
    "VTables": [[]]
  })");
  EXPECT_FALSE(static_cast<bool>(InvalidRange));
  llvm::consumeError(InvalidRange.takeError());

  auto InvalidVertex = DIBasedTypeHierarchyData::loadJsonString(R"({
    "VertexTypes": ["A"],
    "TransitiveDerivedIndex": [[0, 1]],
    "Hierarchy": [1],
    "VTables": [[]]
  })");
  EXPECT_FALSE(static_cast<bool>(InvalidVertex));
  llvm::consumeError(InvalidVertex.takeError());

  auto MismatchedSizes = DIBasedTypeHierarchyData::loadJsonString(R"({
    "VertexTypes": ["A"],
    "TransitiveDerivedIndex": [],
    "Hierarchy": [],
    "VTables": [[]]
  })");
  EXPECT_FALSE(static_cast<bool>(MismatchedSizes));
  llvm::consumeError(MismatchedSizes.takeError());
}

TEST(TypeHierarchyAnalysisTest, CalculateIsIdempotent) {
  LLVMContext Context;
  Module M("type-hierarchy-test", Context);
  StructType::create(Context, "class.A");
  TypeHierarchyAnalysis Analysis(M);

  Analysis.calculate();
  std::string FirstStats;
  raw_string_ostream FirstOS(FirstStats);
  Analysis.printStats(FirstOS);
  FirstOS.flush();

  Analysis.calculate();
  std::string SecondStats;
  raw_string_ostream SecondOS(SecondStats);
  Analysis.printStats(SecondOS);
  SecondOS.flush();

  EXPECT_EQ(SecondStats, FirstStats);
}

} // namespace
