#include "Utils/Graph/DotWriter.h"

#include "IR/ICFG/CallGraph.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGEdge.h"
#include "IR/ICFG/ICFGNode.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace utils {
namespace graphviz {
namespace {

std::string sanitizeClusterName(llvm::StringRef name) {
  std::string result;
  result.reserve(name.size() + 8);
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
      result.push_back(c);
    else
      result.push_back('_');
  }
  if (result.empty())
    result = "anon";
  return "cluster_" + result;
}

void writeNodeAttributes(llvm::raw_ostream &os, const DotNode &node) {
  os << " [label=" << quote(node.label);
  if (!node.shape.empty())
    os << ", shape=" << quote(node.shape);
  if (!node.style.empty())
    os << ", style=" << quote(node.style);
  if (!node.color.empty())
    os << ", color=" << quote(node.color);
  if (!node.tooltip.empty())
    os << ", tooltip=" << quote(node.tooltip);
  os << "]";
}

void writeEdgeAttributes(llvm::raw_ostream &os, const DotEdge &edge) {
  os << " [";
  bool first = true;
  auto attribute = [&](llvm::StringRef name, const std::string &value) {
    if (value.empty())
      return;
    if (!first)
      os << ", ";
    os << name << "=" << quote(value);
    first = false;
  };
  attribute("label", edge.label);
  attribute("style", edge.style);
  attribute("color", edge.color);
  attribute("tooltip", edge.tooltip);
  os << "]";
}

/// Single-line rendering of an instruction for edge labels/tooltips.
std::string instructionLabel(const llvm::Value *instruction) {
  if (!instruction)
    return {};
  std::string text;
  llvm::raw_string_ostream os(text);
  instruction->print(os);
  os.flush();
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.erase(text.begin());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  constexpr std::size_t kMaxLength = 96;
  if (text.size() > kMaxLength)
    text = text.substr(0, kMaxLength) + "...";
  return text;
}

std::string icfgNodeId(const ICFGNode *node) {
  return "N" + std::to_string(node ? node->getId() : -1);
}

bool writeToFile(const DotGraph &dot, llvm::StringRef path,
                 std::string *error) {
  return dot.writeToFile(path, error);
}

DotGraph buildICFG(const ICFG &graph, const DotOptions &options) {
  DotGraph dot("ICFG");
  dot.setLabel("Lotus ICFG");

  std::unordered_map<const llvm::Function *, DotSubgraph *> clusters;
  std::unordered_set<const ICFGNode *> emitted;
  for (const auto &entry : graph) {
    const ICFGNode *node = entry.second;
    if (!node)
      continue;
    if (options.functionFilter && node->getFunction() != options.functionFilter)
      continue;
    emitted.insert(node);
    DotNode out;
    out.id = icfgNodeId(node);
    out.label =
        options.simple ? std::to_string(node->getId()) : node->toString();
    out.shape = "box";

    const llvm::Function *function = node->getFunction();
    if (options.clusterByFunction && function) {
      auto it = clusters.find(function);
      if (it == clusters.end()) {
        DotSubgraph &subgraph = dot.addSubgraph(
            sanitizeClusterName(function->getName()),
            function->getName().empty() ? "<anonymous>"
                                        : function->getName().str());
        it = clusters.emplace(function, &subgraph).first;
      }
      it->second->nodes.push_back(std::move(out));
      continue;
    }
    dot.addNode(std::move(out.id), std::move(out.label)).shape = "box";
  }

  for (const auto &entry : graph) {
    const ICFGNode *node = entry.second;
    if (!node)
      continue;
    for (const ICFGEdge *edge : node->getOutEdges()) {
      if (!edge || !edge->getSrcNode() || !edge->getDstNode())
        continue;
      if (!emitted.count(edge->getSrcNode()) ||
          !emitted.count(edge->getDstNode()))
        continue;
      DotEdge &out = dot.addEdge(
          icfgNodeId(edge->getSrcNode()), icfgNodeId(edge->getDstNode()),
          options.simple ? std::to_string(edge->getEdgeKind())
                         : edge->toString());
      if (edge->isCallCFGEdge())
        out.color = "darkgreen";
      else if (edge->isRetCFGEdge())
        out.color = "darkred";
      else if (edge->isExcRetCFGEdge())
        out.color = "orange";
    }
  }
  return dot;
}

DotGraph buildCFG(const llvm::Function &function, const DotOptions &options) {
  DotGraph dot("CFG_" + (function.getName().empty()
                             ? std::string("anon")
                             : function.getName().str()));
  dot.setLabel(function.getName().empty() ? "<anonymous>"
                                          : function.getName().str());

  std::map<const llvm::BasicBlock *, std::string> ids;
  unsigned index = 0;
  for (const llvm::BasicBlock &block : function) {
    const std::string name =
        block.hasName() ? block.getName().str() : "bb" + std::to_string(index);
    ids.emplace(&block, "B" + std::to_string(index));
    DotNode &node = dot.addNode(ids.at(&block), name);
    if (!options.simple && options.instructionText)
      if (const llvm::Instruction *terminator = block.getTerminator())
        node.tooltip = instructionLabel(terminator);
    ++index;
  }

  for (const llvm::BasicBlock &block : function) {
    const llvm::Instruction *terminator = block.getTerminator();
    unsigned successorIndex = 0;
    for (const llvm::BasicBlock *successor : llvm::successors(&block)) {
      std::string label;
      if (terminator) {
        if (const auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
          if (branch->isConditional())
            label = successorIndex == 0 ? "true" : "false";
        } else if (const auto *switchInst =
                       llvm::dyn_cast<llvm::SwitchInst>(terminator)) {
          if (successorIndex == 0) {
            label = "default";
          } else {
            auto caseIt = switchInst->case_begin();
            std::advance(caseIt, successorIndex - 1);
            std::string text;
            llvm::raw_string_ostream caseOs(text);
            caseIt->getCaseValue()->printAsOperand(caseOs, false);
            caseOs.flush();
            label = text;
          }
        }
      }
      dot.addEdge(ids.at(&block), ids.at(successor), std::move(label));
      ++successorIndex;
    }
  }
  return dot;
}

DotGraph buildCallGraph(const LTCallGraph &graph, const DotOptions &options) {
  DotGraph dot("CallGraph");
  dot.setLabel("Lotus call graph");

  std::unordered_map<const llvm::Function *, std::string> ids;
  auto idFor = [&](const llvm::Function *function) {
    const auto it = ids.find(function);
    if (it != ids.end())
      return it->second;
    const std::string id = "F" + std::to_string(ids.size());
    ids.emplace(function, id);
    return id;
  };

  for (const auto &entry : graph) {
    const llvm::Function *function = entry.first;
    dot.addNode(idFor(function), function && !function->getName().empty()
                                     ? function->getName().str()
                                     : "<anonymous>");
  }

  for (const auto &entry : graph) {
    const LTCallGraphNode *caller = entry.second.get();
    if (!caller)
      continue;
    for (const LTCallGraphNode::CallRecord &record : *caller) {
      const LTCallGraphNode *calleeNode = record.second;
      if (!calleeNode || !calleeNode->getFunction())
        continue;
      const llvm::Function *callee = calleeNode->getFunction();
      const std::string calleeId = idFor(callee);
      dot.addNode(calleeId, callee->getName().empty()
                                ? "<anonymous>"
                                : callee->getName().str());
      DotEdge &edge =
          dot.addEdge(idFor(entry.first), calleeId,
                      options.simple ? "call" : instructionLabel(record.first));
      if (!callee->isDeclaration())
        edge.color = "darkgreen";
    }
  }
  return dot;
}

DotGraph buildSVFG(const analysis::SVFG &graph, const DotOptions &options) {
  DotGraph dot("SVFG");
  dot.setLabel("Lotus SVFG");

  std::unordered_map<std::string, DotSubgraph *> clusters;
  std::unordered_set<const analysis::SVFGNode *> emitted;
  for (const auto &entry : graph) {
    const analysis::SVFGNode *node = entry.second;
    if (!node)
      continue;
    const llvm::Function *function = node->getFunction();
    if (options.functionFilter && function != options.functionFilter)
      continue;
    emitted.insert(node);

    DotNode out;
    out.id = "N" + std::to_string(node->getId());
    if (options.simple) {
      out.label =
          std::to_string(node->getId()) + ":" +
          std::to_string(static_cast<std::uint32_t>(node->getNodeKind()));
    } else {
      std::string label =
          "ID: " + std::to_string(node->getId()) + "\nKind: " +
          std::to_string(static_cast<std::uint32_t>(node->getNodeKind()));
      if (const llvm::Value *value = node->getValue())
        if (value->hasName())
          label += "\nVal: " + value->getName().str();
      std::string functionDebug = graph.getNodeFunctionDebug(node->getId());
      if (functionDebug.empty() && function)
        functionDebug = function->getName().str();
      if (!functionDebug.empty())
        label += "\nFn: " + functionDebug;
      const std::string callDebug = graph.getNodeCallSiteDebug(node->getId());
      if (!callDebug.empty())
        label += "\nCS: " + callDebug;
      out.label = std::move(label);
    }

    std::string functionDebug = graph.getNodeFunctionDebug(node->getId());
    if (functionDebug.empty() && function)
      functionDebug = function->getName().str();
    if (options.clusterByFunction && !functionDebug.empty()) {
      auto it = clusters.find(functionDebug);
      if (it == clusters.end()) {
        DotSubgraph &subgraph =
            dot.addSubgraph(sanitizeClusterName(functionDebug), functionDebug);
        it = clusters.emplace(functionDebug, &subgraph).first;
      }
      it->second->nodes.push_back(std::move(out));
      continue;
    }
    dot.addNode(std::move(out.id), std::move(out.label)).shape = "box";
  }

  for (const auto &entry : graph) {
    const analysis::SVFGNode *node = entry.second;
    if (!node)
      continue;
    for (const analysis::SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || !edge->getSrcNode() || !edge->getDstNode())
        continue;
      if (!emitted.count(edge->getSrcNode()) ||
          !emitted.count(edge->getDstNode()))
        continue;
      std::string label =
          options.simple
              ? std::to_string(static_cast<std::uint32_t>(edge->getEdgeKind()))
              : edge->toString();
      if (!options.simple && edge->hasCallSiteDebug())
        label += "\n" + edge->getCallSiteDebug();
      DotEdge &out = dot.addEdge(
          "N" + std::to_string(edge->getSrcNode()->getId()),
          "N" + std::to_string(edge->getDstNode()->getId()), std::move(label));
      if (!options.simple) {
        if (edge->hasCallSite() || edge->hasCallSiteDebug())
          out.color = "darkgreen";
        if (!edge->getPointsTo().empty())
          out.tooltip = "pts=" + std::to_string(edge->getPointsTo().size());
        if (edge->getWeight() == analysis::SVFGEdge::EdgeWeight::Many)
          out.style = "dashed";
        else if (edge->isThreadMHPEdge())
          out.style = "dotted";
      }
    }
  }
  return dot;
}

} // namespace

std::string escapeLabel(llvm::StringRef text) {
  std::string result;
  result.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      break;
    default:
      result.push_back(c);
      break;
    }
  }
  return result;
}

std::string quote(llvm::StringRef text) {
  return "\"" + escapeLabel(text) + "\"";
}

DotGraph::DotGraph(std::string name, std::string rankdir)
    : name_(std::move(name)), rankdir_(std::move(rankdir)) {}

DotNode &DotGraph::addNode(std::string id, std::string label) {
  DotNode node;
  node.id = std::move(id);
  node.label = label.empty() ? node.id : std::move(label);
  nodes_.push_back(std::move(node));
  return nodes_.back();
}

DotEdge &DotGraph::addEdge(std::string source, std::string target,
                           std::string label) {
  DotEdge edge;
  edge.source = std::move(source);
  edge.target = std::move(target);
  edge.label = std::move(label);
  edges_.push_back(std::move(edge));
  return edges_.back();
}

DotSubgraph &DotGraph::addSubgraph(std::string name, std::string label) {
  DotSubgraph subgraph;
  subgraph.name = std::move(name);
  subgraph.label = std::move(label);
  subgraphs_.push_back(std::move(subgraph));
  return subgraphs_.back();
}

std::size_t DotGraph::nodeCount() const {
  std::size_t count = nodes_.size();
  for (const DotSubgraph &subgraph : subgraphs_)
    count += subgraph.nodes.size();
  return count;
}

std::string DotGraph::str() const {
  std::string text;
  llvm::raw_string_ostream os(text);
  write(os);
  os.flush();
  return text;
}

void DotGraph::write(llvm::raw_ostream &os) const {
  os << "digraph " << quote(name_) << " {\n";
  os << "  rankdir=" << rankdir_ << ";\n";
  if (!label_.empty())
    os << "  label=" << quote(label_) << ";\n";
  if (!nodeDefaults_.empty())
    os << "  node [" << nodeDefaults_ << "];\n";

  for (const DotNode &node : nodes_) {
    os << "  " << quote(node.id);
    writeNodeAttributes(os, node);
    os << ";\n";
  }

  for (const DotSubgraph &subgraph : subgraphs_) {
    os << "  subgraph " << quote(subgraph.name) << " {\n";
    if (!subgraph.label.empty())
      os << "    label=" << quote(subgraph.label) << ";\n";
    for (const DotNode &node : subgraph.nodes) {
      os << "    " << quote(node.id);
      writeNodeAttributes(os, node);
      os << ";\n";
    }
    os << "  }\n";
  }

  for (const DotEdge &edge : edges_) {
    os << "  " << quote(edge.source) << " -> " << quote(edge.target);
    writeEdgeAttributes(os, edge);
    os << ";\n";
  }

  os << "}\n";
}

bool DotGraph::writeToFile(llvm::StringRef path, std::string *error) const {
  std::error_code ec;
  llvm::raw_fd_ostream file(path, ec, llvm::sys::fs::OF_Text);
  if (ec) {
    if (error)
      *error = ec.message();
    return false;
  }
  write(file);
  file.close();
  if (file.has_error()) {
    if (error)
      *error = file.error().message();
    return false;
  }
  return true;
}

bool writeICFGDot(const ICFG &graph, llvm::raw_ostream &os,
                  const DotOptions &options) {
  buildICFG(graph, options).write(os);
  return true;
}

bool writeICFGDot(const ICFG &graph, llvm::StringRef path,
                  const DotOptions &options, std::string *error) {
  return writeToFile(buildICFG(graph, options), path, error);
}

bool writeCallGraphDot(const LTCallGraph &graph, llvm::raw_ostream &os,
                       const DotOptions &options) {
  buildCallGraph(graph, options).write(os);
  return true;
}

bool writeCallGraphDot(const LTCallGraph &graph, llvm::StringRef path,
                       const DotOptions &options, std::string *error) {
  return writeToFile(buildCallGraph(graph, options), path, error);
}

bool writeCFGDot(const llvm::Function &function, llvm::raw_ostream &os,
                 const DotOptions &options) {
  buildCFG(function, options).write(os);
  return true;
}

bool writeCFGDot(const llvm::Function &function, llvm::StringRef path,
                 const DotOptions &options, std::string *error) {
  return writeToFile(buildCFG(function, options), path, error);
}

bool writeSVFGDot(const analysis::SVFG &graph, llvm::raw_ostream &os,
                  const DotOptions &options) {
  buildSVFG(graph, options).write(os);
  return true;
}

bool writeSVFGDot(const analysis::SVFG &graph, llvm::StringRef path,
                  const DotOptions &options, std::string *error) {
  return writeToFile(buildSVFG(graph, options), path, error);
}

namespace {

std::string normalizeFunctionName(llvm::StringRef name) {
  name = name.trim();
  if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
    name = name.drop_front().drop_back();
  name.consume_front("@");
  return name.str();
}

} // namespace

bool writeSVFGDot(const analysis::SVFG &graph, const llvm::Module &module,
                  llvm::StringRef functionName, llvm::StringRef path,
                  const DotOptions &options, std::string *error) {
  DotOptions filtered = options;
  if (!functionName.empty()) {
    const llvm::Function *function =
        module.getFunction(normalizeFunctionName(functionName));
    if (!function || function->isDeclaration()) {
      if (error)
        *error = "unknown function definition '" + functionName.str() + "'";
      return false;
    }
    filtered.functionFilter = function;
  }
  return writeSVFGDot(graph, path, filtered, error);
}

} // namespace graphviz
} // namespace utils
} // namespace lotus
