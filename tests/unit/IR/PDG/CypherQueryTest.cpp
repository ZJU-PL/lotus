#include "IR/PDG/Construction/GraphBuilder.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "IR/PDG/Cypher/Cypher.h"
#include "TestUtils/LLVMHelpers.h"

#include <iostream>
#include <sstream>

#include <gtest/gtest.h>

using namespace llvm;
using namespace pdg;

class CypherQueryTest : public ::testing::Test {
protected:
  void SetUp() override {
    context_ = std::make_unique<LLVMContext>();

    std::string testIR = R"(
      define i32 @test_func(i32 %x) {
      entry:
        %cmp = icmp sgt i32 %x, 0
        br i1 %cmp, label %then, label %else
      then:
        %add = add i32 %x, 1
        ret i32 %add
      else:
        ret i32 0
      }
    )";

    module_ = lotus::unittest::parseModuleChecked(*context_, testIR,
                                                  "CypherQueryTest");
    if (module_) {
      pdg_ = &ProgramGraph::getInstance();
      pdg_->reset();
      pdg::GraphBuilder(*pdg_).build(*module_);
    }
  }

  void TearDown() override {
    if (pdg_ != nullptr)
      pdg_->reset();
    module_.reset();
    context_.reset();
    pdg_ = nullptr;
  }

  ProgramGraph *pdg_ = nullptr;
  std::unique_ptr<LLVMContext> context_;
  std::unique_ptr<Module> module_;
};

class CypherQuerySyntheticTest : public ::testing::Test {
protected:
  void SetUp() override {
    pdg_ = &ProgramGraph::getInstance();
    pdg_->reset();
  }

  void TearDown() override {
    if (pdg_ != nullptr)
      pdg_->reset();
    pdg_ = nullptr;
  }

  Node *addNode(GraphNodeType type) {
    auto *node = new Node(type);
    pdg_->addNode(*node);
    nodes_.push_back(node);
    return node;
  }

  Edge *addEdge(Node *src, Node *dst, EdgeType type) {
    auto *edge = new Edge(src, dst, type);
    src->addOutEdge(*edge);
    dst->addInEdge(*edge);
    pdg_->addEdge(*edge);
    edges_.push_back(edge);
    return edge;
  }

  ProgramGraph *pdg_ = nullptr;
  std::vector<Node *> nodes_;
  std::vector<Edge *> edges_;
};

TEST_F(CypherQueryTest, ParseSimple) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("MATCH (n) RETURN n");

  ASSERT_NE(query, nullptr);
  EXPECT_FALSE(query->getPatterns().empty());
  EXPECT_FALSE(query->getReturnItems().empty());
}

TEST_F(CypherQueryTest, ParseMatchWithLabel) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n:INST_FUNCALL) RETURN n");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getStartNode(), nullptr);
  EXPECT_EQ(pattern->getStartNode()->getLabel(), "INST_FUNCALL");
}

TEST_F(CypherQueryTest, ParseMatchWithRelationship) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[r:EDGE_TYPE]->(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getRelationship(), nullptr);
  EXPECT_EQ(pattern->getRelationship()->getType(), "EDGE_TYPE");
}

TEST_F(CypherQueryTest, ParseMultipleMatchClauses) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a) MATCH (b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->getPatterns().size(), 2u);
}

TEST_F(CypherQueryTest, ParseMultipleWhereClauses) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse(
      "MATCH (a) WHERE a.name = 'x' MATCH (b) WHERE b.name = 'y' RETURN b");

  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->getPatterns().size(), 2u);
  ASSERT_NE(query->getWhereClause(), nullptr);
  EXPECT_TRUE(query->getWhereClause()->isBooleanOp());
}

TEST_F(CypherQueryTest, ParseMatchWithWhere) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) WHERE n.type = 'INST_FUNCALL' RETURN n");

  ASSERT_NE(query, nullptr);
  EXPECT_NE(query->getWhereClause(), nullptr);
}

TEST_F(CypherQueryTest, ParseMatchWithOrderBy) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) RETURN n ORDER BY n.id DESC");

  ASSERT_NE(query, nullptr);
  ASSERT_NE(query->getOrderBy(), nullptr);
  EXPECT_EQ(query->getOrderBy()->getDirection(),
            CypherOrderBy::Direction::DESC);
}

TEST_F(CypherQueryTest, ParseMatchWithLimit) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) RETURN n LIMIT 10");

  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->getLimit(), 10);
}

TEST_F(CypherQueryTest, ParseMatchWithVariableLength) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[*]->(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getRelationship(), nullptr);
  EXPECT_EQ(pattern->getRelationship()->getMinHops(), 1);
  EXPECT_EQ(pattern->getRelationship()->getMaxHops(), -1);
}

TEST_F(CypherQueryTest, ParseErrorHandling) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("INVALID QUERY SYNTAX");

  EXPECT_TRUE(parser.hasError());
}

TEST_F(CypherQueryTest, ExecuteSimpleMatch) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);

  std::unique_ptr<CypherQuery> query = parser.parse("MATCH (n) RETURN n");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(result->isEmpty());
}

TEST_F(CypherQueryTest, ExecuteMatchWithLabel) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);

  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n:FUNC_ENTRY) RETURN n");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(result->isEmpty());
}

TEST_F(CypherQueryTest, QueryResultOperations) {
  CypherResult result(CypherResult::ResultType::NODES);

  EXPECT_TRUE(result.isEmpty());
  result.setScalarValue("test");
  EXPECT_FALSE(result.isEmpty());
  EXPECT_EQ(result.getScalarValue(), "test");
}

TEST_F(CypherQueryTest, CypherResultToString) {
  CypherResult nodesResult(CypherResult::ResultType::NODES);
  std::string nodesStr = nodesResult.toString();
  EXPECT_TRUE(nodesStr.find("nodes") != std::string::npos);

  CypherResult scalarResult(CypherResult::ResultType::SCALAR);
  scalarResult.setScalarValue("hello");
  EXPECT_EQ(scalarResult.toString(), "hello");
}

TEST_F(CypherQueryTest, EmptyQuery) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("");

  EXPECT_EQ(query, nullptr);
  EXPECT_TRUE(parser.hasError());
}

TEST_F(CypherQueryTest, UnclosedParenthesis) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("MATCH (n RETURN n");

  EXPECT_EQ(query, nullptr);
  EXPECT_TRUE(parser.hasError());
}

TEST_F(CypherQueryTest, CaseInsensitiveKeywords) {
  CypherParser parser;

  std::unique_ptr<CypherQuery> queryLower = parser.parse("match (n) return n");
  EXPECT_NE(queryLower, nullptr);

  std::unique_ptr<CypherQuery> queryUpper = parser.parse("MATCH (n) RETURN n");
  EXPECT_NE(queryUpper, nullptr);
}

TEST_F(CypherQueryTest, BidirectionalRelationship) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)<-[r]-(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getRelationship(), nullptr);
  EXPECT_EQ(pattern->getRelationship()->getDirection(),
            CypherRelationshipPattern::Direction::IN);
  EXPECT_FALSE(pattern->getRelationship()->isBidirectional());
}

TEST_F(CypherQueryTest, MultipleReturnItems) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a), (b) RETURN a, b, a.id, b.name");

  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->getReturnItems().size(), 4);
}

TEST_F(CypherQueryTest, ParseComparisonOperators) {
  CypherParser parser;

  auto q1 = parser.parse("MATCH (n) WHERE n.type <= 10 RETURN n");
  ASSERT_NE(q1, nullptr);

  auto q2 = parser.parse("MATCH (n) WHERE n.type >= 10 RETURN n");
  ASSERT_NE(q2, nullptr);

  auto q3 = parser.parse("MATCH (n) WHERE n.type != 10 RETURN n");
  ASSERT_NE(q3, nullptr);
}

TEST_F(CypherQueryTest, ParseChainedPattern) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[r1]->(b)-[r2]->(c) RETURN a, b, c");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());
  const auto *root = query->getPatterns()[0].get();
  ASSERT_NE(root, nullptr);
  ASSERT_NE(root->getRelationship(), nullptr);
  ASSERT_FALSE(root->getNextElements().empty());
  ASSERT_NE(root->getNextElements()[0]->getRelationship(), nullptr);
}

TEST_F(CypherQueryTest, ParsePathBindingPattern) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH p = (a)-[*]->(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());
}

TEST_F(CypherQueryTest, ReturnRelationshipProjection) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[r]->(b) RETURN r LIMIT 5");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::RELATIONSHIPS);
}

TEST_F(CypherQueryTest, ReturnPropertyProjection) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) RETURN n.label LIMIT 3");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::SCALAR);
  EXPECT_FALSE(result->getScalarValue().empty());
}

TEST_F(CypherQueryTest, ScalarProjectionPreservesRowOrderAndLimit) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query = parser.parse(
      "MATCH (n) WHERE EXISTS(n.opcode) RETURN n.opcode ORDER BY n.opcode ASC "
      "LIMIT 4");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::SCALAR);
  EXPECT_EQ(result->getScalarValue(), "add\nbr\nicmp\nret");
}

TEST_F(CypherQueryTest, ScalarProjectionKeepsDuplicateRows) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n:INST_RET) RETURN n.label LIMIT 2");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::SCALAR);
  EXPECT_EQ(result->getScalarValue(), "INST_RET\nINST_RET");
}

TEST_F(CypherQueryTest, FunctionEntryNamePropertyMatchesFunction) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query = parser.parse(
      "MATCH (n:FUNC_ENTRY) WHERE n.name = 'test_func' RETURN n.name");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::SCALAR);
  EXPECT_EQ(result->getScalarValue(), "test_func");
}

TEST_F(CypherQuerySyntheticTest, WhereUsesPerRowBindingsForExists) {
  Node *a1 = addNode(GraphNodeType::INST_OTHER);
  Node *a2 = addNode(GraphNodeType::INST_BR);
  Node *b1 = addNode(GraphNodeType::FUNC_ENTRY);
  Node *b2 = addNode(GraphNodeType::FUNC_ENTRY);
  addEdge(a1, b1, EdgeType::DATA_DEF_USE);
  addEdge(a2, b2, EdgeType::DATA_DEF_USE);

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query = parser.parse(
      "MATCH (a)-[r]->(b) WHERE EXISTS(r) AND a.label = 'INST_OTHER' "
      "RETURN b.label");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::SCALAR);
  EXPECT_EQ(result->getScalarValue(), "FUNC_ENTRY");
}

TEST_F(CypherQuerySyntheticTest, ParserDoesNotMutateConstInput) {
  const std::string query_text = "   MATCH (n) RETURN n   ";

  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse(query_text);

  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query_text, "   MATCH (n) RETURN n   ");
}

TEST_F(CypherQuerySyntheticTest, MatchesAllEdgeTypesAndExactMinHops) {
  Node *a = addNode(GraphNodeType::INST_OTHER);
  Node *b = addNode(GraphNodeType::INST_OTHER);
  Node *c = addNode(GraphNodeType::FUNC_ENTRY);
  Node *cls = addNode(GraphNodeType::CLASS);
  Node *mth = addNode(GraphNodeType::FUNC);
  Node *ann = addNode(GraphNodeType::ANNO_GLOBAL);

  // Triangle A -> B -> C and A -> C so C is reachable in both 1 and 2 hops.
  addEdge(a, b, EdgeType::DATA_DEF_USE);
  addEdge(b, c, EdgeType::DATA_DEF_USE);
  addEdge(a, c, EdgeType::DATA_DEF_USE);

  // Additional PDG edge types previously missing from CypherExecutor.
  addEdge(a, b, EdgeType::PARAMETER_FIELD);
  addEdge(cls, mth, EdgeType::CLS_MTH);
  addEdge(ann, c, EdgeType::ANNO_GLOBAL);

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);

  // [*2..2] MUST reach C via A->B->C even though C is also 1 hop from A.
  auto q2 = parser.parse(
      "MATCH (x:INST_OTHER)-[:DATA_DEF_USE*2..2]->(y:FUNC_ENTRY) RETURN y");
  ASSERT_NE(q2, nullptr);
  auto r2 = executor.execute(*q2);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(r2->getNodes().size(), 1u);
  EXPECT_EQ(r2->getNodes()[0], c);

  // Inverted hop range returns empty without error or hang.
  auto qInv = parser.parse("MATCH (x)-[*3..1]->(y) RETURN y");
  ASSERT_NE(qInv, nullptr);
  auto rInv = executor.execute(*qInv);
  ASSERT_NE(rInv, nullptr);
  EXPECT_TRUE(rInv->getNodes().empty());

  // Newly wired edge types match cleanly.
  for (const char *edge_name : {"PARAMETER_FIELD", "CLS_MTH", "ANNO_GLOBAL"}) {
    auto qEdge = parser.parse(std::string("MATCH (x)-[:") + edge_name +
                              "]->(y) RETURN y");
    ASSERT_NE(qEdge, nullptr) << edge_name;
    auto rEdge = executor.execute(*qEdge);
    ASSERT_NE(rEdge, nullptr) << edge_name;
    EXPECT_EQ(rEdge->getNodes().size(), 1u) << edge_name;
  }
}

TEST_F(CypherQuerySyntheticTest, MalformedWithAndLimitOverflowFailCleanly) {
  CypherParser parser;
  // Regression: malformed WITH return item previously looped infinitely.
  EXPECT_EQ(parser.parse("MATCH (n) WITH COUNT( RETURN n"), nullptr);
  EXPECT_EQ(parser.parse("MATCH (n) WITH n."), nullptr);
  EXPECT_EQ(parser.parse("MATCH (n) RETURN n LIMIT 9999999999999999999999"),
            nullptr);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
