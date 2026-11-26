#include "IR/ICFG/CallGraph.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

namespace {

/// Identity of an abstract object that survives renumbering: the LLVM value
/// the object was created for, its stable debug label, or its (base, offset)
/// pair.
std::string objectIdentity(const SVFG &graph, uint32_t id, int depth = 0) {
  if (const Value *value = graph.getObjectValue(id)) {
    std::string text;
    raw_string_ostream os(text);
    value->printAsOperand(os, false);
    return "V:" + os.str();
  }
  const auto &debug = graph.getObjectDebugMap();
  const auto debugIt = debug.find(id);
  if (debugIt != debug.end() && !debugIt->second.empty())
    return "D:" + debugIt->second;
  const SVFG::ObjectInfo *info = graph.getObjectInfo(id);
  if (info && depth < 8)
    return "B:" + objectIdentity(graph, info->baseObjId, depth + 1) + "@" +
           std::to_string(info->fieldOffset);
  return "?";
}

std::set<std::string> objectIdentities(const SVFG &graph,
                                       const SVFGNodeBS &set) {
  std::set<std::string> result;
  for (uint32_t id : set)
    result.insert(objectIdentity(graph, id));
  return result;
}

using NodeGuards = std::map<uint32_t, std::set<std::string>>;
using EdgeGuards =
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::set<std::string>>;

NodeGuards collectNodeGuards(const SVFG &graph) {
  NodeGuards guards;
  for (const auto &entry : graph) {
    const SVFGNode *node = entry.second;
    if (const SVFGNodeBS *pts = node->getPointsTo())
      if (!pts->empty())
        guards.emplace(node->getId(), objectIdentities(graph, *pts));
  }
  return guards;
}

EdgeGuards collectEdgeGuards(const SVFG &graph) {
  EdgeGuards guards;
  for (const auto &entry : graph) {
    const SVFGNode *node = entry.second;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      if (edge->getPointsTo().empty())
        continue;
      const auto key = std::make_tuple(
          edge->getSrcNode()->getId(), edge->getDstNode()->getId(),
          static_cast<uint32_t>(edge->getEdgeKind()));
      auto &bucket = guards[key];
      const auto identities = objectIdentities(graph, edge->getPointsTo());
      bucket.insert(identities.begin(), identities.end());
    }
  }
  return guards;
}

std::map<const llvm::Value *, std::set<std::string>>
collectValueObjects(const SVFG &graph, const Module &module) {
  std::map<const llvm::Value *, std::set<std::string>> result;
  for (const Function &function : module) {
    for (const BasicBlock &block : function) {
      for (const Instruction &instruction : block) {
        if (!instruction.getType()->isPointerTy())
          continue;
        std::set<std::string> objects =
            objectIdentities(graph, graph.getObjectIds(&instruction));
        if (const uint32_t single = graph.getObjectId(&instruction))
          objects.insert(objectIdentity(graph, single));
        result.emplace(&instruction, std::move(objects));
      }
    }
  }
  return result;
}

class SVFGBuilderObjectClusteringTest : public LlvmModuleTest {
protected:
  static std::unique_ptr<SVFG>
  buildSVFG(Module *module, ICFG &icfg,
            SVFGBuilderConfig::ObjectIdClustering clustering,
            bool usePointerAnalysis = false) {
    ICFGBuilder icfgBuilder(&icfg);
    icfgBuilder.build(module);

    SVFGBuilderConfig cfg;
    cfg.usePointerAnalysis = usePointerAnalysis;
    cfg.buildMSSA = true;
    cfg.pointerOnly = false;
    cfg.objectIdClustering = clustering;

    SVFGBuilder builder(cfg);
    return std::unique_ptr<SVFG>(builder.build(&icfg));
  }
};

} // namespace

TEST_F(SVFGBuilderObjectClusteringTest,
       ExplicitPermutationKeepsGuardsSemantic) {
  const char *source = R"(
    @global = global i8 0

    define i32 @main(i1 %cond) {
    entry:
      %x = alloca [8 x i8]
      %p = getelementptr [8 x i8], [8 x i8]* %x, i64 0, i64 3
      store i8 1, i8* %p
      br i1 %cond, label %left, label %right

    left:
      store i8 2, i8* %p
      br label %join

    right:
      store i8 3, i8* %p
      br label %join

    join:
      %v = load i8, i8* %p
      store i8 4, i8* @global
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ICFG icfg;
  auto svfg = buildSVFG(module.get(), icfg,
                        SVFGBuilderConfig::ObjectIdClustering::None);
  ASSERT_NE(svfg, nullptr);

  const NodeGuards nodeGuardsBefore = collectNodeGuards(*svfg);
  const EdgeGuards edgeGuardsBefore = collectEdgeGuards(*svfg);
  const auto valueObjectsBefore = collectValueObjects(*svfg, *module);
  const std::vector<uint32_t> referenced = svfg->collectReferencedObjectIds();
  ASSERT_FALSE(referenced.empty());

  std::unordered_map<uint32_t, std::pair<uint32_t, uint64_t>> infoBefore;
  for (uint32_t id : referenced) {
    if (const SVFG::ObjectInfo *info = svfg->getObjectInfo(id))
      infoBefore.emplace(id,
                         std::make_pair(info->baseObjId, info->fieldOffset));
  }

  // Reverse permutation over the object namespace.
  std::unordered_map<uint32_t, uint32_t> permutation;
  std::unordered_map<uint32_t, uint32_t> inverse;
  for (std::size_t i = 0; i < referenced.size(); ++i) {
    const uint32_t mapped =
        SVFG::kObjectIdBase + static_cast<uint32_t>(referenced.size() - 1 - i);
    permutation.emplace(referenced[i], mapped);
    inverse.emplace(mapped, referenced[i]);
  }
  const uint32_t maxMapped =
      SVFG::kObjectIdBase + static_cast<uint32_t>(referenced.size() - 1);

  std::string error;
  ASSERT_TRUE(svfg->remapObjectIds(permutation, &error)) << error;

  EXPECT_EQ(svfg->getObjectIdMapping().size(), permutation.size());
  for (const auto &entry : permutation)
    EXPECT_EQ(svfg->getObjectIdMapping().at(entry.first), entry.second);

  // Guards are compared through stable object identities, so a bijective
  // renumbering must not change any of them.
  EXPECT_EQ(collectNodeGuards(*svfg), nodeGuardsBefore);
  EXPECT_EQ(collectEdgeGuards(*svfg), edgeGuardsBefore);
  EXPECT_EQ(collectValueObjects(*svfg, *module), valueObjectsBefore);

  // Base/field invariants survive: every base pointer must point at the
  // renumbered base and the offset must be unchanged.
  for (uint32_t oldId : referenced) {
    const auto before = infoBefore.find(oldId);
    if (before == infoBefore.end())
      continue;
    const SVFG::ObjectInfo *info = svfg->getObjectInfo(permutation.at(oldId));
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->fieldOffset, before->second.second);
    if (before->second.first != 0)
      EXPECT_EQ(info->baseObjId, permutation.at(before->second.first));
    else
      EXPECT_EQ(info->baseObjId, 0u);
  }

  // A fresh allocation must not collide with the remapped namespace.
  const uint32_t fresh = svfg->allocateObjectId();
  EXPECT_GT(fresh, maxMapped);

  // Reverting with the inverse mapping restores the original identities.
  ASSERT_TRUE(svfg->remapObjectIds(inverse, &error)) << error;
  EXPECT_EQ(collectNodeGuards(*svfg), nodeGuardsBefore);
  EXPECT_EQ(collectEdgeGuards(*svfg), edgeGuardsBefore);
  EXPECT_EQ(collectValueObjects(*svfg, *module), valueObjectsBefore);
}

TEST_F(SVFGBuilderObjectClusteringTest, RejectsNonBijectiveMapping) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %x = alloca i8
      %y = alloca i8
      store i8 1, i8* %x
      store i8 2, i8* %y
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ICFG icfg;
  auto svfg = buildSVFG(module.get(), icfg,
                        SVFGBuilderConfig::ObjectIdClustering::None);
  ASSERT_NE(svfg, nullptr);

  const std::vector<uint32_t> referenced = svfg->collectReferencedObjectIds();
  ASSERT_GE(referenced.size(), 2u);

  std::unordered_map<uint32_t, uint32_t> collision;
  for (uint32_t id : referenced)
    collision.emplace(id, SVFG::kObjectIdBase);

  std::string error;
  EXPECT_FALSE(svfg->remapObjectIds(collision, &error));
  EXPECT_FALSE(error.empty());

  // A missing ID is also rejected.
  std::unordered_map<uint32_t, uint32_t> incomplete;
  for (std::size_t i = 0; i + 1 < referenced.size(); ++i)
    incomplete.emplace(referenced[i],
                       SVFG::kObjectIdBase + static_cast<uint32_t>(i));
  error.clear();
  EXPECT_FALSE(svfg->remapObjectIds(incomplete, &error));
  EXPECT_FALSE(error.empty());
}

TEST_F(SVFGBuilderObjectClusteringTest, HierarchicalBuilderMatchesIdentity) {
  const char *source = R"(
    define i32 @main(i1 %cond) {
    entry:
      %x = alloca [4 x i8]
      %y = alloca [4 x i8]
      %p = getelementptr [4 x i8], [4 x i8]* %x, i64 0, i64 1
      store i8 1, i8* %p
      br i1 %cond, label %a, label %b

    a:
      store [4 x i8] zeroinitializer, [4 x i8]* %y
      br label %join

    b:
      store i8 3, i8* %p
      br label %join

    join:
      %v = load i8, i8* %p
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG plainIcfg;
  auto plain = buildSVFG(module.get(), plainIcfg,
                         SVFGBuilderConfig::ObjectIdClustering::None);
  ASSERT_NE(plain, nullptr);

  ICFG clusteredIcfg;
  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;
  cfg.pointerOnly = false;
  cfg.objectIdClustering = SVFGBuilderConfig::ObjectIdClustering::Hierarchical;
  ICFGBuilder icfgBuilder(&clusteredIcfg);
  icfgBuilder.build(module.get());
  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> clustered(builder.build(&clusteredIcfg));
  ASSERT_NE(clustered, nullptr);

  EXPECT_EQ(collectNodeGuards(*clustered), collectNodeGuards(*plain));
  EXPECT_EQ(collectEdgeGuards(*clustered), collectEdgeGuards(*plain));
  EXPECT_EQ(collectValueObjects(*clustered, *module),
            collectValueObjects(*plain, *module));

  // Every abstract object keeps its metadata (heap/stack/global flags).
  const std::vector<uint32_t> plainIds = plain->collectReferencedObjectIds();
  const std::vector<uint32_t> clusteredIds =
      clustered->collectReferencedObjectIds();
  EXPECT_EQ(clusteredIds.size(), plainIds.size());
  for (uint32_t id : clusteredIds)
    EXPECT_NE(clustered->getObjectInfo(id), nullptr);
}

TEST_F(SVFGBuilderObjectClusteringTest,
       HierarchicalBuilderClustersAliasingObjects) {
  // A select chain makes the final pointer points-to all 80 allocas, which is
  // large enough to trigger the fastcluster region path.
  constexpr unsigned kObjects = 80;
  std::string source = "define i32 @main(i1 %cond) {\nentry:\n";
  for (unsigned i = 0; i < kObjects; ++i)
    source += "  %a" + std::to_string(i) + " = alloca i8\n";
  std::string previous = "%a0";
  for (unsigned i = 1; i < kObjects; ++i) {
    source += "  %s" + std::to_string(i) + " = select i1 %cond, i8* " +
              previous + ", i8* %a" + std::to_string(i) + "\n";
    previous = "%s" + std::to_string(i);
  }
  source += "  store i8 1, i8* " + previous + "\n";
  source += "  ret i32 0\n}\n";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG plainIcfg;
  auto plain = buildSVFG(module.get(), plainIcfg,
                         SVFGBuilderConfig::ObjectIdClustering::None, true);
  ASSERT_NE(plain, nullptr);

  ICFG clusteredIcfg;
  ICFGBuilder icfgBuilder(&clusteredIcfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = true;
  cfg.buildMSSA = true;
  cfg.pointerOnly = false;
  cfg.objectIdClustering = SVFGBuilderConfig::ObjectIdClustering::Hierarchical;
  SVFGBuilder builder(cfg);
  std::unique_ptr<SVFG> clustered(builder.build(&clusteredIcfg));
  ASSERT_NE(clustered, nullptr);

  EXPECT_EQ(collectNodeGuards(*clustered), collectNodeGuards(*plain));
  EXPECT_EQ(collectEdgeGuards(*clustered), collectEdgeGuards(*plain));
  EXPECT_EQ(collectValueObjects(*clustered, *module),
            collectValueObjects(*plain, *module));

  const auto &stats = builder.getObjectIdClusteringStatistics();
  ASSERT_GT(stats.objects, 0u);
  EXPECT_GT(stats.clusteredRegions, 0u);
  EXPECT_LE(stats.clusteredWordSpan, stats.originalWordSpan);
  EXPECT_FALSE(stats.fellBackToIdentity);
}
