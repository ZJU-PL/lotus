/**
 * @file ThreadFlowGraph.cpp
 * @brief Implementation of Thread Flow Graph classes
 *
 * The Thread Flow Graph (TFG) is a graph representation of the concurrent
 * program. Nodes represent synchronization events or instructions. Edges
 * represent:
 * 1. Intra-thread control flow (program order)
 * 2. Inter-thread synchronization (fork, join, signal, etc.)
 */

#include "Concurrency/Core/ThreadFlowGraph.h"

#include <cassert>
#include <functional>
#include <queue>
#include <unordered_set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace {

constexpr size_t kInlineEdgeThreshold = 16;

uint32_t edgeKindBit(EdgeKind kind) {
  return uint32_t{1} << static_cast<unsigned>(kind);
}

} // namespace

// ============================================================================
// SyncNode Implementation
// ============================================================================

void SyncNode::addPredecessor(SyncNode *pred) {
  if (!pred) {
    return;
  }
  if (m_predecessor_set) {
    if (m_predecessor_set->insert(pred).second) {
      m_predecessors.push_back(pred);
    }
    return;
  }
  for (SyncNode *existing : m_predecessors) {
    if (existing == pred) {
      return;
    }
  }
  m_predecessors.push_back(pred);
  if (m_predecessors.size() > kInlineEdgeThreshold) {
    m_predecessor_set = std::make_unique<std::unordered_set<SyncNode *>>(
        m_predecessors.begin(), m_predecessors.end());
  }
}

void SyncNode::addSuccessor(SyncNode *succ) {
  addSuccessorWithKind(succ, 0);
}

void SyncNode::addSuccessorWithKind(SyncNode *succ, uint32_t kind_bit) {
  if (!succ) {
    return;
  }
  m_outgoing_edge_kinds |= kind_bit;
  if (m_successor_index) {
    auto it = m_successor_index->find(succ);
    if (it != m_successor_index->end()) {
      m_successor_edge_masks[it->second] |= kind_bit;
      return;
    }
    const size_t idx = m_successors.size();
    m_successors.push_back(succ);
    m_successor_edge_masks.push_back(kind_bit);
    m_successor_index->emplace(succ, idx);
    return;
  }
  for (size_t i = 0; i < m_successors.size(); ++i) {
    if (m_successors[i] == succ) {
      m_successor_edge_masks[i] |= kind_bit;
      return;
    }
  }
  const size_t idx = m_successors.size();
  m_successors.push_back(succ);
  m_successor_edge_masks.push_back(kind_bit);
  if (m_successors.size() > kInlineEdgeThreshold) {
    m_successor_index =
        std::make_unique<std::unordered_map<const SyncNode *, size_t>>();
    m_successor_index->reserve(m_successors.size() * 2);
    for (size_t i = 0; i < m_successors.size(); ++i) {
      m_successor_index->emplace(m_successors[i], i);
    }
  }
  (void)idx;
}

uint32_t SyncNode::getEdgeMaskTo(const SyncNode *succ) const {
  if (!succ) {
    return 0;
  }
  if (m_successor_index) {
    auto it = m_successor_index->find(succ);
    return it != m_successor_index->end() ? m_successor_edge_masks[it->second]
                                          : 0;
  }
  for (size_t i = 0; i < m_successors.size(); ++i) {
    if (m_successors[i] == succ) {
      return m_successor_edge_masks[i];
    }
  }
  return 0;
}

void SyncNode::print(raw_ostream &os) const {
  os << "SyncNode[" << m_node_id << "]: ";
  os << "Type=" << getSyncNodeTypeName(m_type);
  os << ", Thread=" << m_thread_id;
  if (m_call_context_id != 0) {
    os << ", Ctx=" << m_call_context_id;
  }

  if (m_instruction) {
    os << ", Inst=";
    m_instruction->print(os);
  }

  if (m_lock_value) {
    os << ", Lock=";
    m_lock_value->printAsOperand(os, false);
  }

  if (m_forked_thread != 0) {
    os << ", ForkedThread=" << m_forked_thread;
  }

  if (m_joined_thread != 0) {
    os << ", JoinedThread=" << m_joined_thread;
  }
}

std::string SyncNode::toString() const {
  std::string str;
  raw_string_ostream os(str);
  print(os);
  return os.str();
}

// ============================================================================
// ThreadFlowGraph Implementation
// ============================================================================

ThreadFlowGraph::~ThreadFlowGraph() {
  for (auto *node : m_all_nodes) {
    delete node;
  }
}

SyncNode *ThreadFlowGraph::createNode(const Instruction *inst,
                                      SyncNodeType type, ThreadID tid,
                                      CallContextID ctx) {
  if (inst) {
    InstThreadKey key{inst, tid, ctx};
    auto it = m_inst_thread_to_node.find(key);
    if (it != m_inst_thread_to_node.end()) {
      assert(it->second->getType() == type &&
             "conflicting node type for an existing TFG identity");
      return it->second;
    }
  }
  auto *node = new SyncNode(inst, type, tid, ctx, m_next_node_id++);
  m_all_nodes.push_back(node);
  m_thread_nodes[tid].push_back(node);
  if (m_index_built) {
    invalidateReachabilityIndex();
  }

  if (inst) {
    InstThreadKey key{inst, tid, ctx};
    m_inst_thread_to_node[key] = node;
    m_inst_to_nodes[inst].push_back(node);
  }

  return node;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst, ThreadID tid,
                                   CallContextID ctx) const {
  if (!inst) {
    return nullptr;
  }
  auto it = m_inst_thread_to_node.find({inst, tid, ctx});
  return it != m_inst_thread_to_node.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst,
                                   ThreadID tid) const {
  if (!inst) {
    return nullptr;
  }
  SyncNode *match = nullptr;
  for (SyncNode *node : getNodes(inst, tid)) {
    if (match) {
      return nullptr;
    }
    match = node;
  }
  return match;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst) const {
  auto it = m_inst_to_nodes.find(inst);
  if (it != m_inst_to_nodes.end() && it->second.size() == 1) {
    return it->second.front();
  }
  return nullptr;
}

SyncNode *ThreadFlowGraph::getNodeByID(size_t node_id) const {
  if (node_id == 0 || node_id > m_all_nodes.size()) {
    return nullptr;
  }
  return m_all_nodes[node_id - 1];
}

std::vector<SyncNode *>
ThreadFlowGraph::getNodes(const Instruction *inst) const {
  auto it = m_inst_to_nodes.find(inst);
  if (it == m_inst_to_nodes.end()) {
    return {};
  }
  return it->second;
}

std::vector<SyncNode *> ThreadFlowGraph::getNodes(const Instruction *inst,
                                                  ThreadID tid) const {
  std::vector<SyncNode *> result;
  auto it = m_inst_to_nodes.find(inst);
  if (it == m_inst_to_nodes.end()) {
    return result;
  }
  for (SyncNode *node : it->second) {
    if (node->getThreadID() == tid) {
      result.push_back(node);
    }
  }
  return result;
}

void ThreadFlowGraph::addThread(ThreadID tid, const Function *entry) {
  m_thread_entries[tid] = entry;
}

const Function *ThreadFlowGraph::getThreadEntry(ThreadID tid) const {
  auto it = m_thread_entries.find(tid);
  return it != m_thread_entries.end() ? it->second : nullptr;
}

std::vector<ThreadID> ThreadFlowGraph::getAllThreads() const {
  std::vector<ThreadID> threads;
  threads.reserve(m_thread_entries.size());
  for (const auto &pair : m_thread_entries) {
    threads.push_back(pair.first);
  }
  std::sort(threads.begin(), threads.end());
  return threads;
}

void ThreadFlowGraph::setThreadEntryNode(ThreadID tid, SyncNode *entry) {
  m_thread_entry_nodes[tid] = entry;
}

void ThreadFlowGraph::setThreadExitNode(ThreadID tid, SyncNode *exit) {
  if (!exit) {
    return;
  }
  std::vector<SyncNode *> &exits = m_thread_exit_nodes[tid];
  if (std::find(exits.begin(), exits.end(), exit) == exits.end()) {
    exits.push_back(exit);
  }
}

SyncNode *ThreadFlowGraph::getThreadEntryNode(ThreadID tid) const {
  auto it = m_thread_entry_nodes.find(tid);
  return it != m_thread_entry_nodes.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getThreadExitNode(ThreadID tid) const {
  auto it = m_thread_exit_nodes.find(tid);
  if (it == m_thread_exit_nodes.end() || it->second.size() != 1) {
    return nullptr;
  }
  return it->second.front();
}

std::vector<SyncNode *>
ThreadFlowGraph::getThreadExitNodes(ThreadID tid) const {
  auto it = m_thread_exit_nodes.find(tid);
  return it != m_thread_exit_nodes.end() ? it->second
                                         : std::vector<SyncNode *>();
}

void ThreadFlowGraph::addIntraThreadEdge(SyncNode *from, SyncNode *to) {
  addEdge(from, to, EdgeKind::Control);
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to) {
  addInterThreadEdge(from, to, EdgeKind::Create);
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to,
                                         EdgeKind kind) {
  addEdge(from, to, kind);
}

void ThreadFlowGraph::addCallEdge(SyncNode *call_site, SyncNode *callee_entry) {
  addEdge(call_site, callee_entry, EdgeKind::Call);
}

void ThreadFlowGraph::addRetEdge(SyncNode *callee_exit, SyncNode *return_site) {
  addEdge(callee_exit, return_site, EdgeKind::Ret);
}

void ThreadFlowGraph::addEdge(SyncNode *from, SyncNode *to, EdgeKind kind) {
  if (!from || !to) {
    return;
  }
  from->addSuccessorWithKind(to, edgeKindBit(kind));
  to->addPredecessor(from);
  if (from->getThreadID() != to->getThreadID()) {
    m_has_inter_thread_edges = true;
  }
  if (m_index_built) {
    invalidateReachabilityIndex();
  }
}

std::optional<EdgeKind> ThreadFlowGraph::getEdgeKind(const SyncNode *from,
                                                     const SyncNode *to) const {
  if (!from || !to) {
    return std::nullopt;
  }
  const uint32_t mask = from->getEdgeMaskTo(to);
  if (mask == 0) {
    return std::nullopt;
  }
  for (EdgeKind kind :
       {EdgeKind::Join, EdgeKind::Create, EdgeKind::Barrier,
        EdgeKind::TaskDepend, EdgeKind::TaskWait, EdgeKind::TaskCompletion,
        EdgeKind::Signal, EdgeKind::Call, EdgeKind::Ret, EdgeKind::Control}) {
    if (mask & edgeKindBit(kind)) {
      return kind;
    }
  }
  return std::nullopt;
}

bool ThreadFlowGraph::hasEdgeKind(const SyncNode *from, const SyncNode *to,
                                  EdgeKind kind) const {
  if (!from || !to || !from->hasAnyOutgoingEdgeKind(kind)) {
    return false;
  }
  return (from->getEdgeMaskTo(to) & edgeKindBit(kind)) != 0;
}

void ThreadFlowGraph::setFunctionExitNode(ThreadID tid,
                                          const llvm::Function *func,
                                          SyncNode *exit_node,
                                          CallContextID ctx) {
  if (!exit_node) {
    return;
  }
  std::vector<SyncNode *> &exits = m_func_exit_nodes[{tid, func, ctx}];
  if (std::find(exits.begin(), exits.end(), exit_node) == exits.end()) {
    exits.push_back(exit_node);
  }
}

SyncNode *ThreadFlowGraph::getFunctionExitNode(ThreadID tid,
                                               const llvm::Function *func,
                                               CallContextID ctx) const {
  auto it = m_func_exit_nodes.find({tid, func, ctx});
  if (it == m_func_exit_nodes.end() || it->second.size() != 1) {
    return nullptr;
  }
  return it->second.front();
}

std::vector<SyncNode *>
ThreadFlowGraph::getFunctionExitNodes(ThreadID tid, const llvm::Function *func,
                                      CallContextID ctx) const {
  auto it = m_func_exit_nodes.find({tid, func, ctx});
  return it != m_func_exit_nodes.end() ? it->second : std::vector<SyncNode *>();
}

std::vector<SyncNode *>
ThreadFlowGraph::getNodesOfType(SyncNodeType type) const {
  std::vector<SyncNode *> result;
  for (auto *node : m_all_nodes) {
    if (node->getType() == type) {
      result.push_back(node);
    }
  }
  return result;
}

std::vector<SyncNode *> ThreadFlowGraph::getNodesInThread(ThreadID tid) const {
  auto it = m_thread_nodes.find(tid);
  return it != m_thread_nodes.end() ? it->second : std::vector<SyncNode *>();
}

void ThreadFlowGraph::print(raw_ostream &os) const {
  os << "Thread Flow Graph:\n";
  os << "==================\n";
  os << "Total Nodes: " << m_all_nodes.size() << "\n";
  os << "Total Threads: " << m_thread_entries.size() << "\n\n";

  for (auto *node : m_all_nodes) {
    node->print(os);
    os << "\n";

    if (!node->getSuccessors().empty()) {
      os << "  Successors: ";
      for (auto *succ : node->getSuccessors()) {
        os << succ->getNodeID() << " ";
      }
      os << "\n";
    }
  }
}

void ThreadFlowGraph::printAsDot(raw_ostream &os) const {
  os << "digraph ThreadFlowGraph {\n";
  os << "  rankdir=TB;\n";
  os << "  node [shape=box];\n\n";

  // Define nodes
  for (auto *node : m_all_nodes) {
    os << "  node" << node->getNodeID() << " [label=\"";
    os << "ID:" << node->getNodeID() << "\\n";
    os << "T:" << node->getThreadID() << "\\n";
    os << getSyncNodeTypeName(node->getType());
    os << "\"];\n";
  }

  os << "\n";

  // Define edges
  for (auto *node : m_all_nodes) {
    for (auto *succ : node->getSuccessors()) {
      os << "  node" << node->getNodeID() << " -> node" << succ->getNodeID();

      // Different colors for different edge types
      if (node->getThreadID() != succ->getThreadID()) {
        os << " [color=red, style=dashed]"; // Inter-thread edge
      } else if (isSynchronizationNode(node->getType())) {
        os << " [color=blue]"; // Synchronization edge
      }

      os << ";\n";
    }
  }

  os << "}\n";
}

void ThreadFlowGraph::dumpToFile(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  printAsDot(file);
  file.close();
}

// ============================================================================
// Reachability Index Implementation
// ============================================================================

void ThreadFlowGraph::buildReachabilityIndex() {
  if (m_index_built) {
    return;
  }

  errs() << "Building TFG reachability index...\n";

  m_topo_order.assign(m_next_node_id, -1);
  m_scc_representative.assign(m_next_node_id, nullptr);
  m_global_topo_order.assign(m_next_node_id, -1);
  m_global_scc_id.assign(m_next_node_id, 0);
  m_thread_reachability.clear();
  m_topo_nodes.clear();

  for (ThreadID tid : getAllThreads()) {
    buildSCCs(tid);
    buildTopologicalOrder(tid);
  }
  buildGlobalIndex();

  m_index_built = true;

  size_t total_indexed = 0;
  for (const auto &entry : m_topo_nodes) {
    total_indexed += entry.second.size();
  }
  errs() << "Indexed " << total_indexed << " nodes across "
         << m_topo_nodes.size() << " threads\n";
}

void ThreadFlowGraph::invalidateReachabilityIndex() {
  m_index_built = false;
  m_topo_order.clear();
  m_topo_nodes.clear();
  m_scc_representative.clear();
  m_global_topo_order.clear();
  m_global_scc_id.clear();
  m_thread_to_bit.clear();
  m_scc_reachable_thread_mask.clear();
  m_thread_reachability.clear();
}

void ThreadFlowGraph::buildTopologicalOrder(ThreadID tid) {
  std::vector<SyncNode *> &order = m_topo_nodes[tid];
  order.clear();

  auto thread_it = m_thread_nodes.find(tid);
  if (thread_it == m_thread_nodes.end() || thread_it->second.empty()) {
    return;
  }
  const std::vector<SyncNode *> &thread_nodes = thread_it->second;

  std::unordered_map<const SyncNode *, size_t> component_index;
  component_index.reserve(thread_nodes.size());
  std::vector<std::vector<SyncNode *>> components;
  for (SyncNode *node : thread_nodes) {
    const SyncNode *representative = m_scc_representative[node->getNodeID()];
    auto insertion = component_index.emplace(representative, components.size());
    if (insertion.second) {
      components.emplace_back();
    }
    components[insertion.first->second].push_back(node);
  }

  std::vector<std::vector<size_t>> successors(components.size());
  std::vector<size_t> in_degree(components.size(), 0);
  std::vector<uint32_t> edge_stamp(components.size(), 0);
  uint32_t stamp = 0;
  for (size_t from_component = 0; from_component < components.size();
       ++from_component) {
    ++stamp;
    for (SyncNode *node : components[from_component]) {
      for (SyncNode *succ : node->getSuccessors()) {
        if (succ->getThreadID() != tid) {
          continue;
        }
        size_t to_component =
            component_index[m_scc_representative[succ->getNodeID()]];
        if (from_component == to_component ||
            edge_stamp[to_component] == stamp) {
          continue;
        }
        edge_stamp[to_component] = stamp;
        successors[from_component].push_back(to_component);
        ++in_degree[to_component];
      }
    }
  }

  std::queue<size_t> ready;
  for (size_t component = 0; component < components.size(); ++component) {
    if (in_degree[component] == 0) {
      ready.push(component);
    }
  }

  order.reserve(thread_nodes.size());
  int rank = 0;
  while (!ready.empty()) {
    size_t component = ready.front();
    ready.pop();
    if (components[component].size() > 1) {
      std::sort(components[component].begin(), components[component].end(),
                [](const SyncNode *lhs, const SyncNode *rhs) {
                  return lhs->getNodeID() < rhs->getNodeID();
                });
    }
    for (SyncNode *node : components[component]) {
      m_topo_order[node->getNodeID()] = rank;
      order.push_back(node);
    }
    ++rank;
    for (size_t successor : successors[component]) {
      if (--in_degree[successor] == 0) {
        ready.push(successor);
      }
    }
  }
}

void ThreadFlowGraph::buildSCCs(ThreadID tid) {
  auto thread_it = m_thread_nodes.find(tid);
  if (thread_it == m_thread_nodes.end() || thread_it->second.empty()) {
    return;
  }
  const std::vector<SyncNode *> &thread_nodes = thread_it->second;
  if (m_visit_stamp.size() < m_next_node_id) {
    m_visit_stamp.assign(m_next_node_id, 0);
  }
  auto nextStamp = [&]() -> uint32_t {
    if (++m_current_stamp == 0) {
      std::fill(m_visit_stamp.begin(), m_visit_stamp.end(), 0);
      m_current_stamp = 1;
    }
    return m_current_stamp;
  };

  uint32_t visit_mark = nextStamp();
  std::vector<SyncNode *> finish_order;
  finish_order.reserve(thread_nodes.size());

  struct DFSFrame {
    SyncNode *node;
    size_t next_successor = 0;
  };
  std::vector<DFSFrame> stack;

  for (SyncNode *root : thread_nodes) {
    if (m_visit_stamp[root->getNodeID()] == visit_mark) {
      continue;
    }
    m_visit_stamp[root->getNodeID()] = visit_mark;
    stack.push_back({root, 0});
    while (!stack.empty()) {
      DFSFrame &frame = stack.back();
      const auto &successors = frame.node->getSuccessors();
      bool descended = false;
      while (frame.next_successor < successors.size()) {
        SyncNode *succ = successors[frame.next_successor++];
        if (succ->getThreadID() == tid &&
            m_visit_stamp[succ->getNodeID()] != visit_mark) {
          m_visit_stamp[succ->getNodeID()] = visit_mark;
          stack.push_back({succ, 0});
          descended = true;
          break;
        }
      }
      if (!descended && frame.next_successor >= successors.size()) {
        finish_order.push_back(frame.node);
        stack.pop_back();
      }
    }
  }

  visit_mark = nextStamp();
  std::vector<SyncNode *> rev_stack;
  for (auto it = finish_order.rbegin(); it != finish_order.rend(); ++it) {
    SyncNode *representative = *it;
    if (m_visit_stamp[representative->getNodeID()] == visit_mark) {
      continue;
    }
    m_visit_stamp[representative->getNodeID()] = visit_mark;
    rev_stack.push_back(representative);
    while (!rev_stack.empty()) {
      SyncNode *node = rev_stack.back();
      rev_stack.pop_back();
      m_scc_representative[node->getNodeID()] = representative;
      for (SyncNode *pred : node->getPredecessors()) {
        if (pred->getThreadID() == tid &&
            m_visit_stamp[pred->getNodeID()] != visit_mark) {
          m_visit_stamp[pred->getNodeID()] = visit_mark;
          rev_stack.push_back(pred);
        }
      }
    }
  }
}

void ThreadFlowGraph::buildGlobalIndex() {
  if (m_all_nodes.empty()) {
    return;
  }

  // Build thread-level reachability graph (usually tiny: O(T) vertices).
  std::unordered_map<ThreadID, std::unordered_set<ThreadID>> thread_adj;
  for (ThreadID tid : getAllThreads()) {
    thread_adj[tid];
  }
  if (m_has_inter_thread_edges) {
    for (SyncNode *node : m_all_nodes) {
      const ThreadID from_tid = node->getThreadID();
      for (SyncNode *succ : node->getSuccessors()) {
        const ThreadID to_tid = succ->getThreadID();
        if (from_tid != to_tid) {
          thread_adj[from_tid].insert(to_tid);
        }
      }
    }
  }
  for (const auto &entry : thread_adj) {
    const ThreadID start_tid = entry.first;
    auto &reach = m_thread_reachability[start_tid];
    reach.insert(start_tid);
    std::vector<ThreadID> q{start_tid};
    for (size_t head = 0; head < q.size(); ++head) {
      auto adj_it = thread_adj.find(q[head]);
      if (adj_it == thread_adj.end()) {
        continue;
      }
      for (ThreadID next : adj_it->second) {
        if (reach.insert(next).second) {
          q.push_back(next);
        }
      }
    }
  }

  // Fast path: without inter-thread edges, global SCC/topo equals per-thread.
  if (!m_has_inter_thread_edges) {
    std::unordered_map<const SyncNode *, size_t> rep_to_id;
    rep_to_id.reserve(m_all_nodes.size());
    size_t next_scc = 1;
    for (SyncNode *node : m_all_nodes) {
      const size_t id = node->getNodeID();
      m_global_topo_order[id] = m_topo_order[id];
      const SyncNode *rep = m_scc_representative[id];
      auto ins = rep_to_id.emplace(rep ? rep : node, next_scc);
      if (ins.second) {
        ++next_scc;
      }
      m_global_scc_id[id] = ins.first->second;
    }
    return;
  }

  // Full global Kosaraju SCC + topological rank over all nodes/edges.
  if (m_visit_stamp.size() < m_next_node_id) {
    m_visit_stamp.assign(m_next_node_id, 0);
  }
  auto nextStamp = [&]() -> uint32_t {
    if (++m_current_stamp == 0) {
      std::fill(m_visit_stamp.begin(), m_visit_stamp.end(), 0);
      m_current_stamp = 1;
    }
    return m_current_stamp;
  };

  uint32_t mark = nextStamp();
  std::vector<SyncNode *> finish_order;
  finish_order.reserve(m_all_nodes.size());
  struct DFSFrame {
    SyncNode *node;
    size_t next_succ = 0;
  };
  std::vector<DFSFrame> stack;
  for (SyncNode *root : m_all_nodes) {
    if (m_visit_stamp[root->getNodeID()] == mark) {
      continue;
    }
    m_visit_stamp[root->getNodeID()] = mark;
    stack.push_back({root, 0});
    while (!stack.empty()) {
      DFSFrame &frame = stack.back();
      const auto &succs = frame.node->getSuccessors();
      bool descended = false;
      while (frame.next_succ < succs.size()) {
        SyncNode *succ = succs[frame.next_succ++];
        if (m_visit_stamp[succ->getNodeID()] != mark) {
          m_visit_stamp[succ->getNodeID()] = mark;
          stack.push_back({succ, 0});
          descended = true;
          break;
        }
      }
      if (!descended && frame.next_succ >= succs.size()) {
        finish_order.push_back(frame.node);
        stack.pop_back();
      }
    }
  }

  mark = nextStamp();
  std::vector<std::vector<SyncNode *>> components;
  std::vector<SyncNode *> rev_stack;
  for (auto it = finish_order.rbegin(); it != finish_order.rend(); ++it) {
    SyncNode *rep = *it;
    if (m_visit_stamp[rep->getNodeID()] == mark) {
      continue;
    }
    const size_t comp_id = components.size();
    components.emplace_back();
    m_visit_stamp[rep->getNodeID()] = mark;
    rev_stack.push_back(rep);
    while (!rev_stack.empty()) {
      SyncNode *node = rev_stack.back();
      rev_stack.pop_back();
      m_global_scc_id[node->getNodeID()] = comp_id + 1;
      components[comp_id].push_back(node);
      for (SyncNode *pred : node->getPredecessors()) {
        if (m_visit_stamp[pred->getNodeID()] != mark) {
          m_visit_stamp[pred->getNodeID()] = mark;
          rev_stack.push_back(pred);
        }
      }
    }
  }

  std::vector<std::vector<size_t>> comp_succs(components.size());
  std::vector<size_t> in_degree(components.size(), 0);
  std::vector<uint32_t> edge_stamp(components.size(), 0);
  uint32_t e_stamp = 0;
  for (size_t from_c = 0; from_c < components.size(); ++from_c) {
    ++e_stamp;
    for (SyncNode *node : components[from_c]) {
      for (SyncNode *succ : node->getSuccessors()) {
        const size_t to_c = m_global_scc_id[succ->getNodeID()] - 1;
        if (from_c == to_c || edge_stamp[to_c] == e_stamp) {
          continue;
        }
        edge_stamp[to_c] = e_stamp;
        comp_succs[from_c].push_back(to_c);
        ++in_degree[to_c];
      }
    }
  }

  std::vector<size_t> topo_comps;
  topo_comps.reserve(components.size());
  std::queue<size_t> ready;
  for (size_t c = 0; c < components.size(); ++c) {
    if (in_degree[c] == 0) {
      ready.push(c);
    }
  }
  int rank = 0;
  while (!ready.empty()) {
    size_t c = ready.front();
    ready.pop();
    topo_comps.push_back(c);
    for (SyncNode *node : components[c]) {
      m_global_topo_order[node->getNodeID()] = rank;
    }
    ++rank;
    for (size_t next_c : comp_succs[c]) {
      if (--in_degree[next_c] == 0) {
        ready.push(next_c);
      }
    }
  }

  // Compute exact per-SCC reachable-thread bitmask (when <= 64 threads) so
  // cross-thread reachability queries prune unreachable branches in O(1).
  m_thread_to_bit.clear();
  m_scc_reachable_thread_mask.clear();
  std::vector<ThreadID> all_threads = getAllThreads();
  if (all_threads.size() <= 64) {
    for (size_t i = 0; i < all_threads.size(); ++i) {
      m_thread_to_bit.emplace(all_threads[i], static_cast<unsigned>(i));
    }
    m_scc_reachable_thread_mask.assign(components.size() + 1, 0);
    for (size_t c = 0; c < components.size(); ++c) {
      uint64_t mask = 0;
      for (SyncNode *node : components[c]) {
        auto bit_it = m_thread_to_bit.find(node->getThreadID());
        if (bit_it != m_thread_to_bit.end()) {
          mask |= (uint64_t{1} << bit_it->second);
        }
      }
      m_scc_reachable_thread_mask[c + 1] = mask;
    }
    for (auto it = topo_comps.rbegin(); it != topo_comps.rend(); ++it) {
      const size_t c = *it;
      uint64_t mask = m_scc_reachable_thread_mask[c + 1];
      for (size_t next_c : comp_succs[c]) {
        mask |= m_scc_reachable_thread_mask[next_c + 1];
      }
      m_scc_reachable_thread_mask[c + 1] = mask;
    }
  }
}

bool ThreadFlowGraph::canNodeReachThread(const SyncNode *node,
                                         ThreadID target_tid) const {
  if (!node) {
    return false;
  }
  const ThreadID src_tid = node->getThreadID();
  if (src_tid == target_tid) {
    return true;
  }
  if (!m_index_built) {
    return true;
  }
  if (!m_has_inter_thread_edges) {
    return false;
  }
  if (!m_scc_reachable_thread_mask.empty()) {
    auto bit_it = m_thread_to_bit.find(target_tid);
    if (bit_it == m_thread_to_bit.end()) {
      return false;
    }
    const size_t id = node->getNodeID();
    if (id < m_global_scc_id.size()) {
      const size_t scc = m_global_scc_id[id];
      if (scc < m_scc_reachable_thread_mask.size()) {
        return (m_scc_reachable_thread_mask[scc] &
                (uint64_t{1} << bit_it->second)) != 0;
      }
    }
  }
  return canThreadReach(src_tid, target_tid);
}

bool ThreadFlowGraph::canReach(const SyncNode *from, const SyncNode *to) const {
  if (!from || !to || from == to) {
    return false;
  }

  // The per-thread index can prove a same-thread path, and with no
  // inter-thread edges it is exact.
  if (m_index_built) {
    if (!canNodeReachThread(from, to->getThreadID())) {
      return false;
    }
    if (from->getThreadID() == to->getThreadID()) {
      if (canReachViaIndex(from, to)) {
        return true;
      }
      if (!m_has_inter_thread_edges) {
        return false;
      }
    }
    const int from_global = getGlobalTopologicalOrder(from);
    const int to_global = getGlobalTopologicalOrder(to);
    if (from_global >= 0 && to_global >= 0 && from_global > to_global &&
        getGlobalSCCID(from) != getGlobalSCCID(to)) {
      return false;
    }
  }

  if (m_visit_stamp.size() < m_next_node_id) {
    m_visit_stamp.assign(m_next_node_id, 0);
  }
  if (++m_current_stamp == 0) {
    std::fill(m_visit_stamp.begin(), m_visit_stamp.end(), 0);
    m_current_stamp = 1;
  }
  const uint32_t stamp = m_current_stamp;
  const int target_global = m_index_built ? getGlobalTopologicalOrder(to) : -1;

  m_bfs_queue.clear();
  m_bfs_queue.push_back(from);
  m_visit_stamp[from->getNodeID()] = stamp;

  for (size_t head = 0; head < m_bfs_queue.size(); ++head) {
    const SyncNode *current = m_bfs_queue[head];
    for (SyncNode *succ : current->getSuccessors()) {
      if (succ == to) {
        return true;
      }
      const size_t sid = succ->getNodeID();
      if (m_visit_stamp[sid] == stamp) {
        continue;
      }
      if (m_index_built) {
        if (!canNodeReachThread(succ, to->getThreadID())) {
          continue;
        }
        if (target_global >= 0 && m_global_topo_order[sid] > target_global) {
          continue;
        }
      }
      m_visit_stamp[sid] = stamp;
      m_bfs_queue.push_back(succ);
    }
  }

  return false;
}

bool ThreadFlowGraph::canReachViaIndex(const SyncNode *from,
                                       const SyncNode *to) const {
  const int from_topo = getTopologicalOrder(from);
  const int to_topo = getTopologicalOrder(to);
  const ThreadID tid = from->getThreadID();

  if (from_topo >= 0 && to_topo >= 0 && from_topo > to_topo) {
    const SyncNode *from_rep = from->getNodeID() < m_scc_representative.size()
                                   ? m_scc_representative[from->getNodeID()]
                                   : nullptr;
    const SyncNode *to_rep = to->getNodeID() < m_scc_representative.size()
                                 ? m_scc_representative[to->getNodeID()]
                                 : nullptr;
    if (!(from_rep && from_rep == to_rep)) {
      return false;
    }
  }

  if (m_visit_stamp.size() < m_next_node_id) {
    m_visit_stamp.assign(m_next_node_id, 0);
  }
  if (++m_current_stamp == 0) {
    std::fill(m_visit_stamp.begin(), m_visit_stamp.end(), 0);
    m_current_stamp = 1;
  }
  const uint32_t stamp = m_current_stamp;

  m_bfs_queue.clear();
  m_bfs_queue.push_back(from);
  m_visit_stamp[from->getNodeID()] = stamp;

  for (size_t head = 0; head < m_bfs_queue.size(); ++head) {
    const SyncNode *current = m_bfs_queue[head];
    for (SyncNode *succ : current->getSuccessors()) {
      if (succ->getThreadID() != tid) {
        continue;
      }
      if (succ == to) {
        return true;
      }
      const size_t sid = succ->getNodeID();
      if (m_visit_stamp[sid] == stamp) {
        continue;
      }
      if (to_topo >= 0 && sid < m_topo_order.size() &&
          m_topo_order[sid] > to_topo) {
        continue;
      }
      m_visit_stamp[sid] = stamp;
      m_bfs_queue.push_back(succ);
    }
  }

  return false;
}

int ThreadFlowGraph::getTopologicalOrder(const SyncNode *node) const {
  if (!node) {
    return -1;
  }
  const size_t id = node->getNodeID();
  return id < m_topo_order.size() ? m_topo_order[id] : -1;
}

int ThreadFlowGraph::getGlobalTopologicalOrder(const SyncNode *node) const {
  if (!node) {
    return -1;
  }
  const size_t id = node->getNodeID();
  return id < m_global_topo_order.size() ? m_global_topo_order[id] : -1;
}

size_t ThreadFlowGraph::getGlobalSCCID(const SyncNode *node) const {
  if (!node) {
    return 0;
  }
  const size_t id = node->getNodeID();
  return id < m_global_scc_id.size() ? m_global_scc_id[id] : 0;
}

bool ThreadFlowGraph::canThreadReach(ThreadID from_tid, ThreadID to_tid) const {
  if (!m_index_built) {
    return true;
  }
  if (from_tid == to_tid) {
    return true;
  }
  if (!m_has_inter_thread_edges) {
    return false;
  }
  auto it = m_thread_reachability.find(from_tid);
  return it != m_thread_reachability.end() && it->second.count(to_tid) != 0;
}

const std::vector<SyncNode *> &
ThreadFlowGraph::getTopologicalOrderNodes(ThreadID tid) const {
  static const std::vector<SyncNode *> empty;
  auto it = m_topo_nodes.find(tid);
  return it != m_topo_nodes.end() ? it->second : empty;
}

// ============================================================================
// Utility Functions
// ============================================================================

namespace mhp {

StringRef getSyncNodeTypeName(SyncNodeType type) {
  switch (type) {
  case SyncNodeType::THREAD_START:
    return "THREAD_START";
  case SyncNodeType::THREAD_FORK:
    return "THREAD_FORK";
  case SyncNodeType::THREAD_JOIN:
    return "THREAD_JOIN";
  case SyncNodeType::THREAD_EXIT:
    return "THREAD_EXIT";
  case SyncNodeType::LOCK_ACQUIRE:
    return "LOCK_ACQUIRE";
  case SyncNodeType::LOCK_RELEASE:
    return "LOCK_RELEASE";
  case SyncNodeType::COND_WAIT:
    return "COND_WAIT";
  case SyncNodeType::COND_SIGNAL:
    return "COND_SIGNAL";
  case SyncNodeType::COND_BROADCAST:
    return "COND_BROADCAST";
  case SyncNodeType::BARRIER_WAIT:
    return "BARRIER_WAIT";
  case SyncNodeType::REGULAR_INST:
    return "REGULAR_INST";
  case SyncNodeType::FUNCTION_CALL:
    return "FUNCTION_CALL";
  case SyncNodeType::FUNCTION_RETURN:
    return "FUNCTION_RETURN";
  }
  return "UNKNOWN";
}

bool isSynchronizationNode(SyncNodeType type) {
  return type == SyncNodeType::LOCK_ACQUIRE ||
         type == SyncNodeType::LOCK_RELEASE ||
         type == SyncNodeType::COND_WAIT || type == SyncNodeType::COND_SIGNAL ||
         type == SyncNodeType::COND_BROADCAST ||
         type == SyncNodeType::BARRIER_WAIT;
}

bool isThreadBoundaryNode(SyncNodeType type) {
  return type == SyncNodeType::THREAD_START ||
         type == SyncNodeType::THREAD_FORK ||
         type == SyncNodeType::THREAD_JOIN || type == SyncNodeType::THREAD_EXIT;
}

} // namespace mhp
