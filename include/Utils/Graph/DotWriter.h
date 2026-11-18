/**
 * @file DotWriter.h
 * @brief Unified Graphviz/DOT export for Lotus graphs.
 *
 * SVF funnels every graph through `DOTGraphTraits`/`GraphWriter`. Lotus had
 * grown per-module writers (SVFG, GVFG, PDG) and no export at all for ICFG,
 * CFG or the call graph. This header provides:
 *
 *  - `DotGraph`, a small ordered DOT model with correct string escaping;
 *  - `write*Dot` adapters for ICFG, CFG, call graph and SVFG.
 *
 * The adapters only read the graph, so they are safe to call from tests and
 * drivers. Any other graph can be exported by filling a `DotGraph` directly.
 */

#pragma once

#include <deque>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
class raw_ostream;
} // namespace llvm

class LTCallGraph;
class ICFG;

namespace lotus {
namespace analysis {
class SVFG;
} // namespace analysis

namespace utils {
namespace graphviz {

/// Escape the contents of a DOT quoted string. Backslashes and quotes are
/// escaped; real newlines become `\n` (DOT's centred line break).
std::string escapeLabel(llvm::StringRef text);

/// Return @p text as a quoted DOT string, including the surrounding quotes.
std::string quote(llvm::StringRef text);

struct DotNode {
  std::string id;
  std::string label;
  std::string shape = "box";
  std::string style;
  std::string color;
  std::string tooltip;
};

struct DotEdge {
  std::string source;
  std::string target;
  std::string label;
  std::string style;
  std::string color;
  std::string tooltip;
};

struct DotSubgraph {
  std::string name;
  std::string label;
  std::vector<DotNode> nodes;
};

/// Ordered DOT graph model. Nodes/edges/subgraphs keep insertion order so
/// diffs between two dumps are stable.
class DotGraph {
public:
  explicit DotGraph(std::string name, std::string rankdir = "TB");

  DotNode &addNode(std::string id, std::string label = {});
  DotEdge &addEdge(std::string source, std::string target,
                   std::string label = {});
  DotSubgraph &addSubgraph(std::string name, std::string label = {});

  void setRankdir(std::string rankdir) { rankdir_ = std::move(rankdir); }
  void setLabel(std::string label) { label_ = std::move(label); }
  void setNodeDefaults(std::string defaults) {
    nodeDefaults_ = std::move(defaults);
  }

  const std::string &name() const { return name_; }
  std::size_t nodeCount() const;
  std::size_t edgeCount() const { return edges_.size() + subgraphEdges_; }

  std::string str() const;
  void write(llvm::raw_ostream &os) const;
  bool writeToFile(llvm::StringRef path, std::string *error = nullptr) const;

private:
  std::string name_;
  std::string rankdir_ = "TB";
  std::string label_;
  std::string nodeDefaults_ = "shape=box, fontsize=10";
  std::vector<DotNode> nodes_;
  std::vector<DotEdge> edges_;
  // A deque keeps references returned by addSubgraph() stable while more
  // subgraphs are appended (adapters hold these pointers).
  std::deque<DotSubgraph> subgraphs_;
  std::size_t subgraphEdges_ = 0;
};

struct DotOptions {
  /// Compact labels: node IDs and edge kinds only.
  bool simple = false;
  /// Group ICFG/SVFG nodes by containing function.
  bool clusterByFunction = true;
  /// Include full instruction text in labels.
  bool instructionText = true;
  /// Restrict ICFG/SVFG export to one function (nullptr exports everything).
  const llvm::Function *functionFilter = nullptr;
};

bool writeICFGDot(const ICFG &graph, llvm::raw_ostream &os,
                  const DotOptions &options = {});
bool writeICFGDot(const ICFG &graph, llvm::StringRef path,
                  const DotOptions &options = {}, std::string *error = nullptr);

bool writeCallGraphDot(const LTCallGraph &graph, llvm::raw_ostream &os,
                       const DotOptions &options = {});
bool writeCallGraphDot(const LTCallGraph &graph, llvm::StringRef path,
                       const DotOptions &options = {},
                       std::string *error = nullptr);

bool writeCFGDot(const llvm::Function &function, llvm::raw_ostream &os,
                 const DotOptions &options = {});
bool writeCFGDot(const llvm::Function &function, llvm::StringRef path,
                 const DotOptions &options = {}, std::string *error = nullptr);

bool writeSVFGDot(const analysis::SVFG &graph, llvm::raw_ostream &os,
                  const DotOptions &options = {});
bool writeSVFGDot(const analysis::SVFG &graph, llvm::StringRef path,
                  const DotOptions &options = {}, std::string *error = nullptr);

/// Write a function-restricted SVFG DOT file. \p functionName accepts `main`,
/// `@main`, or `"main"`. An empty name exports the whole graph.
bool writeSVFGDot(const analysis::SVFG &graph, const llvm::Module &module,
                  llvm::StringRef functionName, llvm::StringRef path,
                  const DotOptions &options = {}, std::string *error = nullptr);

} // namespace graphviz
} // namespace utils
} // namespace lotus
