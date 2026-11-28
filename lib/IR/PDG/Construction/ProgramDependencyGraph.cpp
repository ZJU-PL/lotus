#include "llvm/Support/CommandLine.h"

#include "IR/PDG/Construction/GraphBuilder.h"
/**
 * @file ProgramDependencyGraph.cpp
 * @brief Implementation of the Program Dependency Graph (PDG) pass
 *
 * This file implements the core PDG pass that builds a complete
 * inter-procedural program dependency graph for LLVM modules. The PDG
 * integrates both control and data dependencies and supports field-sensitive,
 * context-insensitive, and flow-insensitive analysis.
 *
 * The PDG construction process involves:
 * 1. Building the call graph
 * 2. Connecting global variables with their uses
 * 3. Connecting intra-procedural control and data dependencies
 * 4. Connecting inter-procedural dependencies across function calls
 * 5. Connecting class nodes with their methods
 *
 * A key feature is the handling of function parameters through "tree"
 * structures that enable field-sensitive parameter analysis.
 */

#include "llvm/ADT/DenseSet.h"

#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "IR/PDG/Core/PDGCommandLineOptions.h"

#include <chrono>

using namespace llvm;

char pdg::ProgramDependencyGraph::ID = 0;

void pdg::ProgramDependencyGraph::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DataDependencyGraph>();
  AU.addRequired<ControlDependencyGraph>();
  AU.setPreservesAll();
}

/**
 * @brief Main entry point for the PDG pass.
 *
 * Initializes the PDG, builds the call graph, and orchestrates the connection
 * of dependencies (global, intra-procedural, inter-procedural).
 *
 * @param M The LLVM Module to analyze.
 * @return false (Analysis passes should return false).
 */
bool pdg::ProgramDependencyGraph::runOnModule(Module &M) {
  auto start = std::chrono::high_resolution_clock::now();
  _module = &M;
  _PDG = &ProgramGraph::getInstance();
  _inst_order_func = nullptr;
  _inst_order_map.clear();

  _call_graph.build(M);

  if (!_PDG->isBuiltForModule(M)) {
    _PDG->reset();
    pdg::GraphBuilder(*_PDG).build(M);
    pdg::GraphBuilder(*_PDG).bindDebugInfo(M);
  }

  unsigned func_size = 0;
  connectGlobalWithUses();
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (!_PDG->hasFuncWrapper(F))
      continue;
    connectIntraprocDependencies(F);
    connectInterprocDependencies(F);
    connectClassNodeWithClassMethods(F);
    func_size++;
  }
  _inst_order_func = nullptr;
  _inst_order_map.clear();
  if (DEBUG) {
    errs() << "func size: " << func_size << "\n";
    errs() << "Finsh adding dependencies" << "\n";
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    errs() << "building PDG takes: " << duration.count() << "\n";
    errs() << "PDG Node size: " << _PDG->numNode() << "\n";
  }

  if (DEBUG)
    _PDG->dumpGraph();

  return false;
}

/**
 * @brief Connects global variables with their uses in the program.
 *
 * Establishes DATA_DEF_USE edges between global variable nodes and the
 * instruction nodes that use them.
 */
void pdg::ProgramDependencyGraph::connectGlobalWithUses() {
  for (auto &global_var : _module->getGlobalList()) {
    Node *n = _PDG->getNode(global_var);
    if (n == nullptr)
      continue;

    for (auto *user : global_var.users()) {
      Node *user_node = _PDG->getNode(*user);
      if (user_node == nullptr)
        continue;
      n->addNeighbor(*user_node, EdgeType::DATA_DEF_USE);
    }
  }
}

/**
 * @brief Connects two parameter trees with incoming edges (e.g., ActualIn ->
 * FormalIn).
 *
 * Traverses both trees in parallel and adds edges between corresponding nodes.
 *
 * @param src_tree The source tree (e.g., Actual Argument Tree).
 * @param dst_tree The destination tree (e.g., Formal Parameter Tree).
 * @param edge_type The type of edge to create (e.g., PARAMETER_IN).
 */
void pdg::ProgramDependencyGraph::connectInTrees(Tree *src_tree, Tree *dst_tree,
                                                 EdgeType edge_type) {
  if (!src_tree || !dst_tree)
    return;
  if (!src_tree->isShapeCompatible(*dst_tree)) {
    // Fallback: at least connect roots when tree shapes differ (e.g., void* vs
    // struct*).
    if (auto *src_root = src_tree->getRootNode())
      if (auto *dst_root = dst_tree->getRootNode())
        src_root->addNeighbor(*dst_root, edge_type);
    return;
  }
  auto *src_tree_root_node = src_tree->getRootNode();
  auto *dst_tree_root_node = dst_tree->getRootNode();
  if (!src_tree_root_node || !dst_tree_root_node)
    return;
  std::queue<std::pair<TreeNode *, TreeNode *>> node_pairs_queue;
  node_pairs_queue.push(std::make_pair(src_tree_root_node, dst_tree_root_node));
  while (!node_pairs_queue.empty()) {
    auto current_node_pair = node_pairs_queue.front();
    node_pairs_queue.pop();
    TreeNode *src = current_node_pair.first;
    TreeNode *dst = current_node_pair.second;
    if (!src || !dst)
      continue;
    src->addNeighbor(*dst, edge_type);
    if (src->numOfChild() != dst->numOfChild())
      continue;
    const auto &src_node_children = src->getChildNodes();
    const auto &dst_node_children = dst->getChildNodes();
    for (int i = 0; i < src->numOfChild(); i++) {
      node_pairs_queue.push(
          std::make_pair(src_node_children[i], dst_node_children[i]));
    }
  }
}

void pdg::ProgramDependencyGraph::connectOutTrees(Tree *src_tree,
                                                  Tree *dst_tree,
                                                  EdgeType edge_type) {
  if (!src_tree || !dst_tree)
    return;
  if (!src_tree->isShapeCompatible(*dst_tree)) {
    // Fallback: connect roots to avoid dropping all PARAMETER_OUT edges.
    if (auto *src_root = src_tree->getRootNode())
      if (auto *dst_root = dst_tree->getRootNode()) {
        // Check addr_vars directly for write access.
        bool has_write = src_root->hasWriteAccess();
        if (!has_write) {
          for (auto *addr_var : src_root->getAddrVars()) {
            if (pdgutils::hasWriteAccess(*addr_var)) {
              src_root->addAccessTag(AccessTag::DATA_WRITE);
              has_write = true;
              break;
            }
          }
        }
        if (has_write)
          src_root->addNeighbor(*dst_root, edge_type);
      }
    return;
  }
  auto *src_tree_root_node = src_tree->getRootNode();
  auto *dst_tree_root_node = dst_tree->getRootNode();
  if (!src_tree_root_node || !dst_tree_root_node)
    return;
  std::queue<std::pair<TreeNode *, TreeNode *>> node_pairs_queue;
  node_pairs_queue.push(std::make_pair(src_tree_root_node, dst_tree_root_node));
  while (!node_pairs_queue.empty()) {
    auto current_node_pair = node_pairs_queue.front();
    node_pairs_queue.pop();
    TreeNode *src = current_node_pair.first;
    TreeNode *dst = current_node_pair.second;
    if (!src || !dst)
      continue;

    // Eagerly compute write-access from addr_vars if the tag is not yet set.
    if (!src->hasWriteAccess()) {
      for (auto *addr_var : src->getAddrVars()) {
        if (pdgutils::hasWriteAccess(*addr_var)) {
          src->addAccessTag(AccessTag::DATA_WRITE);
          break;
        }
      }
    }
    if (src->hasWriteAccess())
      src->addNeighbor(*dst, edge_type);

    if (src->numOfChild() != dst->numOfChild())
      continue;
    const auto &src_node_children = src->getChildNodes();
    const auto &dst_node_children = dst->getChildNodes();
    for (int i = 0; i < src->numOfChild(); i++) {
      node_pairs_queue.push(
          std::make_pair(src_node_children[i], dst_node_children[i]));
    }
  }
}

void pdg::ProgramDependencyGraph::connectCallerAndCallee(
    CallWrapper &cw, FunctionWrapper &fw, EdgeType call_edge_type) {
  connectCallerAndCalleeInternal(cw, fw, call_edge_type, true);
}

void pdg::ProgramDependencyGraph::connectCallerAndCalleeInternal(
    CallWrapper &cw, FunctionWrapper &fw, EdgeType call_edge_type,
    bool attach_actual_trees) {
  // step 1: connect call site node with the entry node of function
  auto *call_site_node = _PDG->getNode(*cw.getCallInst());
  auto *func_entry_node = fw.getEntryNode();
  if (call_site_node == nullptr || func_entry_node == nullptr)
    return;
  call_site_node->addNeighbor(*func_entry_node, call_edge_type);

  // step 2: connect actual in -> formal in, formal out -> actual out
  const auto &actual_arg_list = cw.getArgList();
  const auto &formal_arg_list = fw.getArgList();
  if (actual_arg_list.size() != formal_arg_list.size())
    return;
  if (DEBUG && cw.getCalledFunc())
    errs() << "connecting interproc call: " << cw.getCalledFunc()->getName()
           << " - " << cw.getCallInst()->getFunction()->getName() << "\n";
  int num_arg = actual_arg_list.size();
  for (int i = 0; i < num_arg; i++) {
    Value *actual_arg = actual_arg_list[i];
    Argument *formal_arg = formal_arg_list[i];
    // step 2: connect actual in -> formal in
    auto *actual_in_tree = cw.getArgActualInTree(*actual_arg);
    auto *formal_in_tree = fw.getArgFormalInTree(*formal_arg);
    if (!actual_in_tree || !formal_in_tree)
      continue;
    if (DEBUG)
      errs() << "tree size compare: " << actual_in_tree->size() << " - "
             << formal_in_tree->size() << "\n";
    if (attach_actual_trees) {
      _PDG->addTreeNodesToGraph(*actual_in_tree);
      connectActualInTreeWithAddrVars(*actual_in_tree, *cw.getCallInst());
    }
    connectInTrees(actual_in_tree, formal_in_tree, EdgeType::PARAMETER_IN);
    // step 3: connect actual out -> formal out
    auto *actual_out_tree = cw.getArgActualOutTree(*actual_arg);
    auto *formal_out_tree = fw.getArgFormalOutTree(*formal_arg);
    if (!actual_out_tree || !formal_out_tree)
      continue;
    if (attach_actual_trees)
      _PDG->addTreeNodesToGraph(*actual_out_tree);
    connectOutTrees(formal_out_tree, actual_out_tree, EdgeType::PARAMETER_OUT);
    if (attach_actual_trees)
      connectActualOutTreeWithAddrVars(*actual_out_tree, *cw.getCallInst());
  }

  // step3: connect return value actual in -> formal in, formal out -> actual
  // out
  if (!fw.hasNullRetVal() && !cw.hasNullRetVal()) {
    Tree *ret_formal_in_tree = fw.getRetFormalInTree();
    Tree *ret_formal_out_tree = fw.getRetFormalOutTree();
    Tree *ret_actual_in_tree = cw.getRetActualInTree();
    Tree *ret_actual_out_tree = cw.getRetActualOutTree();
    if (ret_formal_in_tree && ret_actual_in_tree) {
      if (attach_actual_trees) {
        _PDG->addTreeNodesToGraph(*ret_actual_in_tree);
        connectActualInTreeWithAddrVars(*ret_actual_in_tree, *cw.getCallInst());
      }
      connectInTrees(ret_actual_in_tree, ret_formal_in_tree,
                     EdgeType::PARAMETER_IN);
    }
    if (ret_formal_out_tree && ret_actual_out_tree) {
      if (attach_actual_trees)
        _PDG->addTreeNodesToGraph(*ret_actual_out_tree);
      connectOutTrees(ret_formal_out_tree, ret_actual_out_tree,
                      EdgeType::PARAMETER_OUT);
      if (attach_actual_trees)
        connectActualOutTreeWithAddrVars(*ret_actual_out_tree,
                                         *cw.getCallInst());
    }
  }

  // step4: connect both control/data return edges of callee to the call site
  const auto &ret_insts = fw.getReturnInsts();
  auto *call_inst = cw.getCallInst();
  auto *dst = _PDG->getNode(*call_inst);
  if (!dst)
    return;
  for (auto *ret_inst : ret_insts) {
    Node *src = _PDG->getNode(*ret_inst);
    if (src == nullptr)
      continue;
    src->addNeighbor(*dst, EdgeType::CONTROLDEP_CALLRET);
    src->addNeighbor(*dst, EdgeType::DATA_RET);
  }
}

// ===== connect dependencies =====
/**
 * @brief Connects intra-procedural dependencies for a function.
 *
 * Adds control dependencies (CDG) and connects formal parameters to their
 * usages within the function (address variables).
 *
 * @param F The function to process.
 */
void pdg::ProgramDependencyGraph::connectIntraprocDependencies(Function &F) {
  // add control dependency edges
  getAnalysis<ControlDependencyGraph>(
      F); // add control dependencies for nodes in F
  // connect formal tree with address variables
  FunctionWrapper *func_w = getFuncWrapper(F);
  if (!func_w)
    return;
  Node *entry_node = func_w->getEntryNode();
  for (auto *arg : func_w->getArgList()) {
    Tree *formal_in_tree = func_w->getArgFormalInTree(*arg);
    if (!formal_in_tree)
      continue;

    Tree *formal_out_tree = func_w->getArgFormalOutTree(*arg);
    if (!formal_out_tree)
      continue;
    if (entry_node && formal_in_tree->getRootNode())
      entry_node->addNeighbor(*formal_in_tree->getRootNode(),
                              EdgeType::PARAMETER_IN);
    if (entry_node && formal_out_tree->getRootNode())
      entry_node->addNeighbor(*formal_out_tree->getRootNode(),
                              EdgeType::PARAMETER_OUT);
    connectFormalInTreeWithAddrVars(*formal_in_tree);
    connectFormalOutTreeWithAddrVars(*formal_out_tree);
  }

  if (!func_w->hasNullRetVal()) {
    if (auto *ret_in = func_w->getRetFormalInTree())
      connectFormalInTreeWithAddrVars(*ret_in);
    if (auto *ret_out = func_w->getRetFormalOutTree())
      connectFormalOutTreeWithAddrVars(*ret_out);
  }
}

void pdg::ProgramDependencyGraph::connectInterprocDependencies(Function &F) {
  auto *func_w = getFuncWrapper(F);
  if (!func_w)
    return;

  ensureInstOrderForFunction(F);

  // Use the call graph for indirect-call candidate discovery, but connect
  // inter-procedural edges per-callsite (rather than per-caller-function).
  PDGCallGraph &call_g = _call_graph;
  const auto &call_insts = func_w->getCallInsts();
  for (auto *call_inst : call_insts) {
    if (_PDG->hasCallWrapper(*call_inst)) {
      auto *call_w = getCallWrapper(*call_inst);
      if (!call_w)
        continue;
      auto *call_site_node = _PDG->getNode(*call_inst);
      if (!call_site_node)
        continue;

      if (auto *direct = call_w->getCalledFunc()) {
        if (auto *callee_fw = getFuncWrapper(*direct)) {
          if (!call_w->hasParamTrees()) {
            call_w->buildActualTreeForArgs(*callee_fw);
            call_w->buildActualTreesForRetVal(*callee_fw);
            call_w->setHasParamTrees();
          }
          connectCallerAndCalleeInternal(*call_w, *callee_fw,
                                         EdgeType::CONTROLDEP_CALLINV, true);
        }
        continue;
      }

      // Indirect call: connect to all signature-compatible candidates.
      // Build and attach actual parameter trees once per call site instead of
      // clearing and re-allocating TreeNodes for every candidate callee.
      if (!_module)
        continue;
      const auto &candidates =
          call_g.getIndirectCallCandidatesFast(*call_inst, *_module);
      bool trees_attached = false;
      for (auto *cand : candidates) {
        if (!cand)
          continue;
        auto *callee_fw = getFuncWrapper(*cand);
        if (!callee_fw)
          continue;
        bool attach_now = false;
        if (!call_w->hasParamTrees()) {
          bool callee_has_trees = !callee_fw->getArgFormalInTreeMap().empty() ||
                                  !callee_fw->hasNullRetVal();
          if (callee_has_trees) {
            call_w->buildActualTreeForArgs(*callee_fw);
            call_w->buildActualTreesForRetVal(*callee_fw);
            call_w->setHasParamTrees();
            attach_now = true;
            trees_attached = true;
          }
        } else if (!trees_attached) {
          attach_now = true;
          trees_attached = true;
        }
        connectCallerAndCalleeInternal(*call_w, *callee_fw, EdgeType::IND_CALL,
                                       attach_now);
      }
    }
  }
}

void pdg::ProgramDependencyGraph::connectClassNodeWithClassMethods(
    Function &F) {
  FunctionWrapper *fw = getFuncWrapper(F);
  if (!fw || !fw->getEntryNode())
    return;
  std::string cls_name = fw->getClassName();
  if (cls_name.empty())
    return;
  auto *class_node = _PDG->getClassNodeByName(cls_name);
  if (class_node == nullptr)
    return;
  class_node->addNeighbor(*fw->getEntryNode(), EdgeType::CLS_MTH);
}

void pdg::ProgramDependencyGraph::connectFormalInTreeWithAddrVars(
    Tree &formal_in_tree) {
  auto *root_node = formal_in_tree.getRootNode();
  if (!root_node)
    return;
  std::queue<TreeNode *> node_queue;
  node_queue.push(root_node);
  while (!node_queue.empty()) {
    TreeNode *current_node = node_queue.front();
    node_queue.pop();
    if (!current_node)
      continue;
    TreeNode *parent_node = current_node->getParentNode();
    const std::unordered_set<Value *> *parent_addr_vars =
        parent_node ? &parent_node->getAddrVars() : nullptr;
    for (auto *addr_var : current_node->getAddrVars()) {
      if (!_PDG->hasNode(*addr_var))
        continue;
      auto *addr_var_node = _PDG->getNode(*addr_var);
      current_node->addNeighbor(*addr_var_node, EdgeType::PARAMETER_IN);
      auto alias_nodes =
          addr_var_node->getOutNeighborsWithDepType(EdgeType::DATA_ALIAS);
      for (auto *alias_node : alias_nodes) {
        Value *alias_node_val = alias_node->getValue();
        if (alias_node_val == nullptr)
          continue;
        if (parent_addr_vars &&
            parent_addr_vars->find(alias_node_val) != parent_addr_vars->end())
          continue;
        current_node->addNeighbor(*alias_node, EdgeType::PARAMETER_IN);
      }
    }

    for (auto *child_node : current_node->getChildNodes()) {
      if (child_node)
        node_queue.push(child_node);
    }
  }
}

void pdg::ProgramDependencyGraph::connectFormalOutTreeWithAddrVars(
    Tree &formal_out_tree) {
  TreeNode *root_node = formal_out_tree.getRootNode();
  if (!root_node)
    return;
  std::queue<TreeNode *> node_queue;
  node_queue.push(root_node);
  while (!node_queue.empty()) {
    TreeNode *current_node = node_queue.front();
    node_queue.pop();
    if (!current_node)
      continue;
    for (auto *addr_var : current_node->getAddrVars()) {
      if (!_PDG->hasNode(*addr_var))
        continue;
      auto *addr_var_node = _PDG->getNode(*addr_var);
      if (pdgutils::hasWriteAccess(*addr_var)) {
        addr_var_node->addNeighbor(*current_node, EdgeType::PARAMETER_OUT);
        current_node->addAccessTag(AccessTag::DATA_WRITE);
      }
    }

    for (auto *child_node : current_node->getChildNodes()) {
      if (child_node)
        node_queue.push(child_node);
    }
  }
}

void pdg::ProgramDependencyGraph::ensureInstOrderForFunction(Function &F) {
  if (_inst_order_func == &F)
    return;
  _inst_order_func = &F;
  _inst_order_map.clear();
  unsigned idx = 0;
  for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F); ++inst_iter) {
    _inst_order_map[&*inst_iter] = idx++;
  }
}

void pdg::ProgramDependencyGraph::connectActualInTreeWithAddrVars(
    Tree &actual_in_tree, CallBase &ci) {
  TreeNode *root_node = actual_in_tree.getRootNode();
  if (!root_node)
    return;

  Function *F = ci.getFunction();
  if (F)
    ensureInstOrderForFunction(*F);
  auto ci_it = _inst_order_map.find(&ci);
  const bool has_ci_order = (ci_it != _inst_order_map.end());
  const unsigned ci_order = has_ci_order ? ci_it->second : 0;

  std::queue<TreeNode *> node_queue;
  node_queue.push(root_node);
  while (!node_queue.empty()) {
    TreeNode *current_node = node_queue.front();
    node_queue.pop();
    if (!current_node)
      continue;
    for (auto *addr_var : current_node->getAddrVars()) {
      // Only connect addr_vars that are defined before the call site.
      // Non-instruction values (Arguments, GlobalVariables) are always visible.
      if (Instruction *i = dyn_cast<Instruction>(addr_var)) {
        if (!has_ci_order)
          continue;
        auto it = _inst_order_map.find(i);
        if (it == _inst_order_map.end() || it->second >= ci_order)
          continue;
      }
      if (!_PDG->hasNode(*addr_var))
        continue;
      auto *addr_var_node = _PDG->getNode(*addr_var);
      addr_var_node->addNeighbor(*current_node, EdgeType::PARAMETER_IN);
    }

    for (auto *child_node : current_node->getChildNodes()) {
      if (child_node)
        node_queue.push(child_node);
    }
  }
}

void pdg::ProgramDependencyGraph::connectActualOutTreeWithAddrVars(
    Tree &actual_out_tree, CallBase &ci) {
  TreeNode *root_node = actual_out_tree.getRootNode();
  if (!root_node)
    return;
  std::queue<TreeNode *> node_queue;
  node_queue.push(root_node);
  while (!node_queue.empty()) {
    TreeNode *current_node = node_queue.front();
    node_queue.pop();
    if (!current_node)
      continue;
    for (auto *addr_var : current_node->getAddrVars()) {
      if (!_PDG->hasNode(*addr_var))
        continue;
      auto *addr_var_node = _PDG->getNode(*addr_var);
      current_node->addNeighbor(*addr_var_node, EdgeType::PARAMETER_OUT);
    }

    for (auto *child_node : current_node->getChildNodes()) {
      if (child_node)
        node_queue.push(child_node);
    }
  }
}

static RegisterPass<pdg::ProgramDependencyGraph>
    PDG("pdg", "Program Dependency Graph Construction", false, true);
