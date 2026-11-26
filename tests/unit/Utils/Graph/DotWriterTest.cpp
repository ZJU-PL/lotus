#include "Utils/Graph/DotWriter.h"

#include "IR/ICFG/CallGraph.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;
using namespace lotus::utils::graphviz;

namespace {

std::string render(const std::function<void(raw_ostream &)> &writer) {
  std::string text;
  raw_string_ostream os(text);
  writer(os);
  os.flush();
  return text;
}

class DotWriterTest : public LlvmModuleTest {};

} // namespace

TEST(DotGraphTest, EscapesQuotesBackslashesAndNewlines) {
  EXPECT_EQ(escapeLabel("plain"), "plain");
  EXPECT_EQ(escapeLabel("a\"b"), "a\\\"b");
  EXPECT_EQ(escapeLabel("a\\b"), "a\\\\b");
  EXPECT_EQ(escapeLabel("a\nb"), "a\\nb");
  EXPECT_EQ(quote("x\""), "\"x\\\"\"");
}

TEST(DotGraphTest, EmitsNodesEdgesAndSubgraphs) {
  DotGraph graph("G");
  graph.addNode("a", "node a");
  graph.addNode("b", "node b").color = "red";
  graph.addEdge("a", "b", "edge\nlabel");
  graph.addSubgraph("cluster_f", "f").nodes.push_back({"c", "node c", "box"});

  const std::string dot = graph.str();
  EXPECT_NE(dot.find("digraph \"G\" {"), std::string::npos);
  EXPECT_NE(dot.find("\"a\" [label=\"node a\", shape=\"box\"];"),
            std::string::npos);
  EXPECT_NE(dot.find("\"b\" [label=\"node b\", shape=\"box\", color=\"red\"];"),
            std::string::npos);
  EXPECT_NE(dot.find("\"a\" -> \"b\" [label=\"edge\\nlabel\"];"),
            std::string::npos);
  EXPECT_NE(dot.find("subgraph \"cluster_f\""), std::string::npos);
  EXPECT_EQ(graph.nodeCount(), 3u);
  EXPECT_EQ(graph.edgeCount(), 1u);

  // Deterministic: rendering twice produces identical bytes.
  EXPECT_EQ(graph.str(), dot);
}

TEST_F(DotWriterTest, ExportsCFGWithBranchLabels) {
  const char *source = R"(
    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %yes, label %no
    yes:
      ret i32 1
    no:
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const std::string dot =
      render([&](raw_ostream &os) { ASSERT_TRUE(writeCFGDot(*mainFn, os)); });
  EXPECT_NE(dot.find("digraph \"CFG_main\""), std::string::npos);
  EXPECT_NE(dot.find("label=\"true\""), std::string::npos);
  EXPECT_NE(dot.find("label=\"false\""), std::string::npos);
}

TEST_F(DotWriterTest, ExportsICFGAndCallGraph) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define i8* @make() {
    entry:
      %p = call i8* @malloc(i64 8)
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %p = call i8* @make()
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  const std::string icfgDot =
      render([&](raw_ostream &os) { ASSERT_TRUE(writeICFGDot(icfg, os)); });
  EXPECT_NE(icfgDot.find("digraph \"ICFG\""), std::string::npos);
  EXPECT_NE(icfgDot.find("->"), std::string::npos);
  EXPECT_NE(icfgDot.find("subgraph"), std::string::npos);

  LTCallGraph callGraph(*module);
  const std::string callDot = render(
      [&](raw_ostream &os) { ASSERT_TRUE(writeCallGraphDot(callGraph, os)); });
  EXPECT_NE(callDot.find("digraph \"CallGraph\""), std::string::npos);
  EXPECT_NE(callDot.find("\"make\""), std::string::npos);
  EXPECT_NE(callDot.find("\"main\""), std::string::npos);
  EXPECT_NE(callDot.find("->"), std::string::npos);
}

TEST_F(DotWriterTest, ManyFunctionSubgraphsRemainValid) {
  std::string source = "declare i8* @malloc(i64)\n";
  for (unsigned i = 0; i < 120; ++i) {
    source += "define i8* @f" + std::to_string(i) + "(i8* %p) {\n";
    source += "entry:\n  %q = call i8* @malloc(i64 8)\n  ret i8* %q\n}\n";
  }
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  const std::string dot =
      render([&](raw_ostream &os) { ASSERT_TRUE(writeICFGDot(icfg, os)); });
  std::size_t subgraphs = 0;
  for (std::size_t pos = 0;
       (pos = dot.find("subgraph", pos)) != std::string::npos; ++pos)
    ++subgraphs;
  EXPECT_GE(subgraphs, 120u);

  // Restricting to one function drops both nodes and inter-function edges.
  DotOptions filtered;
  filtered.functionFilter = module->getFunction("f7");
  ASSERT_NE(filtered.functionFilter, nullptr);
  const std::string filteredDot = render(
      [&](raw_ostream &os) { ASSERT_TRUE(writeICFGDot(icfg, os, filtered)); });
  EXPECT_NE(filteredDot.find("subgraph \"cluster_f7\""), std::string::npos);
  EXPECT_EQ(filteredDot.find("subgraph \"cluster_f8\""), std::string::npos);
}

TEST_F(DotWriterTest, ExportsSVFGWithoutBreakingRequestedFormat) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %x = alloca i8
      store i8 1, i8* %x
      %v = load i8, i8* %x
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig config;
  config.usePointerAnalysis = false;
  config.buildMSSA = true;
  config.pointerOnly = false;
  SVFGBuilder svfgBuilder(config);
  std::unique_ptr<SVFG> graph(svfgBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  DotOptions simple;
  simple.simple = true;
  const std::string simpleDot = render(
      [&](raw_ostream &os) { ASSERT_TRUE(writeSVFGDot(*graph, os, simple)); });
  EXPECT_NE(simpleDot.find("digraph \"SVFG\""), std::string::npos);
  EXPECT_NE(simpleDot.find("->"), std::string::npos);
  EXPECT_EQ(simpleDot.find("Kind: "), std::string::npos);

  const std::string verboseDot =
      render([&](raw_ostream &os) { ASSERT_TRUE(writeSVFGDot(*graph, os)); });
  EXPECT_NE(verboseDot.find("Kind: "), std::string::npos);
  EXPECT_NE(simpleDot, verboseDot);
}

TEST_F(DotWriterTest, FunctionFilteredSVFGDump) {
  const char *source = R"(
    define void @helper(i8** %p) {
    entry:
      store i8* null, i8** %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8*
      call void @helper(i8** %x)
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());

  SVFGBuilderConfig config;
  config.usePointerAnalysis = false;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  auto readFile = [](const std::string &path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  };

  std::string error;
  SmallString<256> helperPath;
  int helperFd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-helper", "dot", helperFd, helperPath));
  ::close(helperFd);
  ASSERT_TRUE(
      writeSVFGDot(*graph, *module, "helper", helperPath.str(), {}, &error))
      << error;
  const std::string helperDot = readFile(helperPath.str().str());
  EXPECT_NE(helperDot.find("cluster_helper"), std::string::npos);
  EXPECT_EQ(helperDot.find("cluster_main"), std::string::npos);
  sys::fs::remove(helperPath);

  SmallString<256> mainPath;
  int mainFd = -1;
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("svfg-main", "dot", mainFd, mainPath));
  ::close(mainFd);
  error.clear();
  ASSERT_TRUE(
      writeSVFGDot(*graph, *module, "@main", mainPath.str(), {}, &error))
      << error;
  const std::string mainDot = readFile(mainPath.str().str());
  EXPECT_NE(mainDot.find("cluster_main"), std::string::npos);
  EXPECT_EQ(mainDot.find("cluster_helper"), std::string::npos);
  sys::fs::remove(mainPath);

  error.clear();
  EXPECT_FALSE(
      writeSVFGDot(*graph, *module, "missing", "unused.dot", {}, &error));
  EXPECT_NE(error.find("missing"), std::string::npos);
}
