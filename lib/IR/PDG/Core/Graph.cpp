/**
 * @file Graph.cpp
 * @brief Implementation of graph data structures for the Program Dependency
 * Graph
 *
 * This file implements the core graph data structures used in the PDG system:
 * - GenericGraph: A base graph class providing common graph operations
 * - ProgramGraph: A specific implementation for program dependency analysis
 *
 * Features include:
 * - Mapping LLVM Values to graph nodes
 * - Graph traversal and reachability analysis
 * - Node and edge creation and management
 * - Supporting field-sensitive analysis through tree structures
 * - Class hierarchy and function call relationship modeling
 *
 * The graph implementation allows for extensive queries about program
 * dependencies, supporting both control and data dependency analyses in an
 * integrated structure.
 */

#include "IR/PDG/Core/Graph.h"

#include "IR/PDG/Core/PDGUtils.h"

#include <stack>

using namespace llvm;

// Generic Graph
bool pdg::GenericGraph::hasNode(Value &v) {
  return (_val_node_map.find(&v) != _val_node_map.end());
}

pdg::Node *pdg::GenericGraph::getNode(Value &v) {
  auto it = _val_node_map.find(&v);
  if (it == _val_node_map.end())
    return nullptr;
  return it->second;
}

int pdg::GenericGraph::numEdge() {
  size_t total = 0;
  for (auto *node : _node_set) {
    if (node)
      total += node->getOutEdgeSet().size();
  }
  return static_cast<int>(std::max(total, _edge_set.size()));
}

// pretty print nodes and edges in PDG
void pdg::GenericGraph::dumpGraph() {
  // print nodes
  errs() << "=============== Node Set ===============\n";
  for (auto node_iter = begin(); node_iter != end(); ++node_iter) {
    auto *node = *node_iter;
    std::string str;
    raw_string_ostream OS(str);
    Value *val = node->getValue();
    if (val != nullptr) {
      if (Function *f = dyn_cast<Function>(val))
        OS << f->getName().str() << "\n";
      else
        OS << *val << "\n";
    }

    if (node->getValue() != nullptr)
      errs() << "node: " << node << " - " << pdgutils::rtrim(OS.str()) << " - "
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
    else
      errs() << "node: " << node << " - "
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
  }

  // print edges
  errs() << "=============== Edge Set ===============\n";
  for (auto node_iter = begin(); node_iter != end(); ++node_iter) {
    auto *node = *node_iter;
    for (auto *out_edge : node->getOutEdgeSet()) {
      errs() << "edge: " << out_edge << " / " << "src["
             << out_edge->getSrcNode() << "] / " << " dst["
             << out_edge->getDstNode() << "]" << " / "
             << pdgutils::getEdgeTypeStr(out_edge->getEdgeType()) << "\n";
    }
  }
}

// ===== Graph Traversal =====
// DFS search
/**
 * @brief Checks if a path exists between two nodes.
 *
 * @param src The source node.
 * @param dst The destination node.
 * @return true if reachable, false otherwise.
 */
bool pdg::GenericGraph::canReach(pdg::Node &src, pdg::Node &dst) {
  // TODO: prune by call graph rechability, improve traverse efficiency
  if (canReach(src, dst, {}))
    return true;
  return false;
}

/**
 * @brief Checks if a path exists between two nodes, excluding specific edge
 * types.
 *
 * Performs a DFS traversal avoiding edges of types listed in
 * exclude_edge_types.
 *
 * @param src The source node.
 * @param dst The destination node.
 * @param exclude_edge_types Set of edge types to ignore during traversal.
 * @return true if reachable, false otherwise.
 */
bool pdg::GenericGraph::canReach(pdg::Node &src, pdg::Node &dst,
                                 std::set<EdgeType> exclude_edge_types) {
  std::unordered_set<Node *> visited;
  std::stack<Node *> node_stack;
  node_stack.push(&src);
  visited.insert(&src);

  while (!node_stack.empty()) {
    auto *current_node = node_stack.top();
    node_stack.pop();
    if (current_node == &dst)
      return true;
    for (auto *out_edge : current_node->getOutEdgeSet()) {
      // exclude path
      if (exclude_edge_types.find(out_edge->getEdgeType()) !=
          exclude_edge_types.end())
        continue;
      auto *next_node = out_edge->getDstNode();
      if (next_node && visited.insert(next_node).second)
        node_stack.push(next_node);
    }
  }
  return false;
}

void pdg::ProgramGraph::addTreeNodesToGraph(pdg::Tree &tree) {
  TreeNode *root_node = tree.getRootNode();
  if (root_node == nullptr)
    return;
  std::queue<TreeNode *> node_queue;
  node_queue.push(root_node);
  while (!node_queue.empty()) {
    TreeNode *current_node = node_queue.front();
    node_queue.pop();
    if (!current_node)
      continue;
    addNode(*current_node);
    for (auto *child_node : current_node->getChildNodes()) {
      if (child_node)
        node_queue.push(child_node);
    }
  }
}

void pdg::ProgramGraph::addFormalTreeNodesToGraph(FunctionWrapper &func_w) {
  for (auto *arg : func_w.getArgList()) {
    Tree *formal_in_tree = func_w.getArgFormalInTree(*arg);
    Tree *formal_out_tree = func_w.getArgFormalOutTree(*arg);
    if (formal_in_tree)
      addTreeNodesToGraph(*formal_in_tree);
    if (formal_out_tree)
      addTreeNodesToGraph(*formal_out_tree);
  }
}

pdg::Node *pdg::ProgramGraph::getClassNodeByName(std::string cls_name) {
  auto it = _class_node_map.find(cls_name);
  if (it == _class_node_map.end())
    return nullptr;
  return _class_node_map[cls_name];
}

pdg::ProgramGraph::~ProgramGraph() { reset(); }

pdg::GenericGraph::~GenericGraph() { reset(); }

void pdg::GenericGraph::reset() {
  bumpEpoch();
  _is_build = false;
  // Collect every edge exactly once via out-edge sets.
  std::unordered_set<Edge *> edges_to_delete;
  for (auto *node : _node_set) {
    if (!node)
      continue;
    for (auto *edge : node->getOutEdgeSet()) {
      if (edge)
        edges_to_delete.insert(edge);
    }
  }
  for (auto *edge : edges_to_delete) {
    delete edge;
  }
  for (auto *node : _node_set) {
    delete node;
  }
  _val_node_map.clear();
  _edge_set.clear();
  _node_set.clear();
}

void pdg::ProgramGraph::reset() {
  // Wrapper destructors release Trees; their TreeNodes remain graph-owned.
  for (auto &entry : _call_wrapper_map) {
    delete entry.second;
  }
  for (auto &entry : _func_wrapper_map) {
    delete entry.second;
  }
  // Delete nodes only after the wrappers that reference them.
  GenericGraph::reset();
  _func_wrapper_map.clear();
  _call_wrapper_map.clear();
  _node_di_type_map.clear();
  _class_node_map.clear();
  _class_name_set.clear();
  _cdg_built_funcs.clear();
  _built_module = nullptr;
}
