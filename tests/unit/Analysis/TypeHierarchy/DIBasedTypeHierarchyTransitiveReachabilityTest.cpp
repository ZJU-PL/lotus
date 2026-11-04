#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "TestUtils/LLVMHelpers.h"

#include <string>

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus;

namespace {

using lotus::unittest::loadModule;
using lotus::unittest::parseModuleChecked;

std::string getTestFilePath(const std::string &FileName) {
  return std::string(LOTUS_TYPE_HIERARCHY_LL_DIR) + "/" + FileName;
}

} // namespace

TEST(DIBasedTypeHierarchyTest, ResolvesInheritanceThroughODRIdentifier) {
  LLVMContext Context;
  auto M = parseModuleChecked(Context, R"(
    source_filename = "canonical-di-type.ll"

    !llvm.module.flags = !{!0}
    !llvm.dbg.cu = !{!1}

    !0 = !{i32 2, !"Debug Info Version", i32 3}
    !1 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !2,
      producer: "test", isOptimized: false, runtimeVersion: 0,
      emissionKind: FullDebug, retainedTypes: !3)
    !2 = !DIFile(filename: "canonical.cpp", directory: ".")
    !3 = !{!4, !5, !7}
    !4 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Base",
      flags: DIFlagFwdDecl, identifier: "_ZTS4Base")
    !5 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Base",
      file: !2, line: 1, size: 8, elements: !6, identifier: "_ZTS4Base")
    !6 = !{}
    !7 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "Derived",
      file: !2, line: 2, size: 8, elements: !8, identifier: "_ZTS7Derived")
    !8 = !{!9}
    !9 = !DIDerivedType(tag: DW_TAG_inheritance, scope: !7, baseType: !4,
      extraData: i32 0)
  )");
  ASSERT_NE(M, nullptr);

  DIBasedTypeHierarchy Hierarchy(*M);
  EXPECT_EQ(Hierarchy.size(), 2u);

  auto Base = Hierarchy.getType("_ZTS4Base");
  auto Derived = Hierarchy.getType("_ZTS7Derived");
  ASSERT_TRUE(Base.has_value());
  ASSERT_TRUE(Derived.has_value());
  EXPECT_FALSE(cast<DICompositeType>(*Base)->isForwardDecl());
  EXPECT_TRUE(Hierarchy.isSubType(*Base, *Derived));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_9) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_9_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, DiamondReachabilityHasNoDuplicates) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_22_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  auto TopType = DBTH.getType("_ZTS3Top");
  auto LeftType = DBTH.getType("_ZTS4Left");
  auto RightType = DBTH.getType("_ZTS5Right");
  auto BottomType = DBTH.getType("_ZTS6Bottom");
  ASSERT_TRUE(TopType.has_value());
  ASSERT_TRUE(LeftType.has_value());
  ASSERT_TRUE(RightType.has_value());
  ASSERT_TRUE(BottomType.has_value());

  auto ReachableTypes = DBTH.subTypesOf(*TopType);
  EXPECT_EQ(std::distance(ReachableTypes.begin(), ReachableTypes.end()), 4);
  EXPECT_EQ(std::set<DIBasedTypeHierarchy::ClassType>(ReachableTypes.begin(),
                                                      ReachableTypes.end())
                .size(),
            4U);
  EXPECT_TRUE(DBTH.isSubType(*TopType, *BottomType));
  EXPECT_FALSE(DBTH.isSubType(*BottomType, *TopType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_10) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_10_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_11) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_11_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_12) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_12_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_12_b) {
  LLVMContext Context;
  auto M =
      loadModule(getTestFilePath("type_hierarchy_12_b_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.has_value());
  auto ChildsChildType = DBTH.getType("_ZTS11ChildsChild");
  ASSERT_TRUE(ChildsChildType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChildsChild = DBTH.getSubTypes(*ChildsChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 3U);
  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesChildsChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildsChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildsChildType));
  EXPECT_TRUE(ReachableTypesChildsChild.count(*ChildsChildType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_12_c) {
  LLVMContext Context;
  auto M =
      loadModule(getTestFilePath("type_hierarchy_12_c_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.has_value());
  auto ChildsChildType = DBTH.getType("_ZTS11ChildsChild");
  ASSERT_TRUE(ChildsChildType.has_value());

  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChildsChild = DBTH.getSubTypes(*ChildsChildType);

  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesChildsChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildsChildType));
  EXPECT_TRUE(ReachableTypesChildsChild.count(*ChildsChildType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_14) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_14_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);

  EXPECT_EQ(ReachableTypesBase.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_15) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_15_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_16) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_16_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());
  auto BaseTwoType = DBTH.getType("_ZTS7BaseTwo");
  ASSERT_TRUE(BaseTwoType.has_value());
  auto ChildTwoType = DBTH.getType("_ZTS8ChildTwo");
  ASSERT_TRUE(ChildTwoType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesBaseTwo = DBTH.getSubTypes(*BaseTwoType);
  auto ReachableTypesChildTwo = DBTH.getSubTypes(*ChildTwoType);

  EXPECT_EQ(ReachableTypesBase.size(), 3U);
  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesBaseTwo.size(), 2U);
  EXPECT_EQ(ReachableTypesChildTwo.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBaseTwo.count(*BaseTwoType));
  EXPECT_TRUE(ReachableTypesBaseTwo.count(*ChildTwoType));
  EXPECT_FALSE(ReachableTypesChildTwo.count(*BaseTwoType));
  EXPECT_TRUE(ReachableTypesChildTwo.count(*ChildTwoType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_17) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_17_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.has_value());
  auto KidType = DBTH.getType("_ZTS3Kid");
  ASSERT_TRUE(KidType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesBase2 = DBTH.getSubTypes(*Base2Type);
  auto ReachableTypesKid = DBTH.getSubTypes(*KidType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_EQ(ReachableTypesBase2.size(), 2U);
  EXPECT_EQ(ReachableTypesKid.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase2.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*KidType));
  EXPECT_FALSE(ReachableTypesKid.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesKid.count(*KidType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_18) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_18_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());
  auto Child3Type = DBTH.getType("_ZTS7Child_3");
  ASSERT_TRUE(Child3Type.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChild3 = DBTH.getSubTypes(*Child3Type);

  EXPECT_EQ(ReachableTypesBase.size(), 4U);
  EXPECT_EQ(ReachableTypesChild.size(), 3U);
  EXPECT_EQ(ReachableTypesChild3.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild3.count(*Child3Type));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_19) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_19_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());
  auto FooType = DBTH.getType("_ZTS3Foo");
  ASSERT_TRUE(FooType.has_value());
  auto BarType = DBTH.getType("_ZTS3Bar");
  ASSERT_TRUE(BarType.has_value());
  auto LoremType = DBTH.getType("_ZTS5Lorem");
  ASSERT_TRUE(LoremType.has_value());
  auto ImpsumType = DBTH.getType("_ZTS6Impsum");
  ASSERT_TRUE(ImpsumType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesFoo = DBTH.getSubTypes(*FooType);
  auto ReachableTypesBar = DBTH.getSubTypes(*BarType);
  auto ReachableTypesLorem = DBTH.getSubTypes(*LoremType);
  auto ReachableTypesImpsum = DBTH.getSubTypes(*ImpsumType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_EQ(ReachableTypesFoo.size(), 2U);
  EXPECT_EQ(ReachableTypesBar.size(), 1U);
  EXPECT_EQ(ReachableTypesLorem.size(), 2U);
  EXPECT_EQ(ReachableTypesImpsum.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesFoo.count(*FooType));
  EXPECT_TRUE(ReachableTypesFoo.count(*BarType));
  EXPECT_FALSE(ReachableTypesBar.count(*FooType));
  EXPECT_TRUE(ReachableTypesBar.count(*BarType));
  EXPECT_TRUE(ReachableTypesLorem.count(*LoremType));
  EXPECT_TRUE(ReachableTypesLorem.count(*ImpsumType));
  EXPECT_FALSE(ReachableTypesImpsum.count(*LoremType));
  EXPECT_TRUE(ReachableTypesImpsum.count(*ImpsumType));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_20) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_20_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesBase2 = DBTH.getSubTypes(*Base2Type);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesBase2.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase2.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_FALSE(ReachableTypesChild.count(*Base2Type));
}
TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_21) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_21_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.has_value());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.has_value());
  auto Base3Type = DBTH.getType("_ZTS5Base3");
  ASSERT_TRUE(Base3Type.has_value());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.has_value());
  auto Child2Type = DBTH.getType("_ZTS6Child2");
  ASSERT_TRUE(Child2Type.has_value());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesBase2 = DBTH.getSubTypes(*Base2Type);
  auto ReachableTypesBase3 = DBTH.getSubTypes(*Base3Type);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChild2 = DBTH.getSubTypes(*Child2Type);

  EXPECT_EQ(ReachableTypesBase.size(), 3U);
  EXPECT_EQ(ReachableTypesBase2.size(), 3U);
  EXPECT_EQ(ReachableTypesBase3.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesChild2.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase.count(*Child2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase2.count(*Child2Type));
  EXPECT_TRUE(ReachableTypesBase3.count(*Base3Type));
  EXPECT_TRUE(ReachableTypesBase3.count(*Child2Type));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*Child2Type));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_FALSE(ReachableTypesChild.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesChild2.count(*Child2Type));
  EXPECT_FALSE(ReachableTypesChild2.count(*BaseType));
  EXPECT_FALSE(ReachableTypesChild2.count(*Base2Type));
  EXPECT_FALSE(ReachableTypesChild2.count(*Base3Type));
}
