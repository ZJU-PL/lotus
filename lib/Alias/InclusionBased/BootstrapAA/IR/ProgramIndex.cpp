#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

void computeKosarajuSCCs(std::size_t n,
                         const std::vector<std::vector<Id>> &successors,
                         const std::vector<std::vector<Id>> &predecessors,
                         std::vector<Id> &component, Id &component_count) {
  std::vector<bool> visited(n, false);
  std::vector<Id> postorder;
  postorder.reserve(n);
  for (Id start = 0; start < n; ++start) {
    if (visited[start])
      continue;
    std::vector<std::pair<Id, std::size_t>> dfs{{start, 0}};
    visited[start] = true;
    while (!dfs.empty()) {
      auto &[node, next] = dfs.back();
      if (next < successors[node].size()) {
        const Id succ = successors[node][next++];
        if (!visited[succ]) {
          visited[succ] = true;
          dfs.emplace_back(succ, 0);
        }
        continue;
      }
      postorder.push_back(node);
      dfs.pop_back();
    }
  }
  component.assign(n, INVALID);
  component_count = 0;
  for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
    if (component[*it] != INVALID)
      continue;
    std::vector<Id> work{*it};
    component[*it] = component_count;
    while (!work.empty()) {
      const Id node = work.back();
      work.pop_back();
      for (Id pred : predecessors[node]) {
        if (component[pred] == INVALID) {
          component[pred] = component_count;
          work.push_back(pred);
        }
      }
    }
    ++component_count;
  }
}

Targets callees(const Program &program, const Instruction &instruction,
                const PointsToSet &target) {
  Targets result;
  result.opaque = instruction.opaque_alternative;
  if (instruction.callee != INVALID) {
    result.functions.insert(instruction.callee);
  } else if (instruction.indirect_target == INVALID || target.isTop()) {
    for (Id f = 0; f < program.functions.size(); ++f)
      result.functions.insert(f);
    result.opaque = true;
  } else {
    for (Id object : target.objects()) {
      if (program.objects[object].kind == ObjectKind::Function)
        result.functions.insert(program.objects[object].function);
      else if (object != NULL_OBJECT)
        result.opaque = true;
    }
  }
  return result;
}

ProgramIndex::ProgramIndex(const Program &program) {
  Id max_site = 0;
  bool any_site = false;
  forInstructions(program, [&](Id, const Instruction &inst) {
    if (!any_site || inst.id > max_site) {
      max_site = inst.id;
      any_site = true;
    }
  });
  by_site.resize(any_site ? static_cast<std::size_t>(max_site) + 1 : 0);
  defining_sites.resize(program.values.size());
  phi_defs.resize(program.values.size());
  return_sites.resize(program.functions.size());
  pred_edges.resize(program.functions.size());
  incoming_calls.resize(program.functions.size());
  in_loop_block.resize(program.functions.size());

  for (Id f = 0; f < program.functions.size(); ++f) {
    const Function &func = program.functions[f];
    pred_edges[f].resize(func.blocks.size());
    std::vector<std::vector<Id>> cfg_succ(func.blocks.size());
    std::vector<std::vector<Id>> cfg_pred(func.blocks.size());
    for (Id b = 0; b < func.blocks.size(); ++b) {
      const Block &block = func.blocks[b];
      for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
        const Instruction &inst = block.instructions[idx];
        by_site[inst.id] = {f, b, idx, &inst};
        if (inst.result != INVALID && inst.opcode != Opcode::Nop &&
            inst.opcode != Opcode::Store && inst.opcode != Opcode::MemCopy &&
            inst.opcode != Opcode::Return)
          defining_sites[inst.result].push_back(inst.id);
        switch (inst.opcode) {
        case Opcode::Store:
          store_sites.push_back(inst.id);
          break;
        case Opcode::MemCopy:
          memcopy_sites.push_back(inst.id);
          break;
        case Opcode::Havoc:
          havoc_sites.push_back(inst.id);
          break;
        case Opcode::Call:
          call_sites.push_back(inst.id);
          break;
        case Opcode::Allocate:
          allocate_sites.push_back(inst.id);
          break;
        case Opcode::Return:
          return_sites[f].push_back(inst.id);
          break;
        default:
          break;
        }
      }
      for (std::size_t e = 0; e < block.successors.size(); ++e) {
        const Edge &edge = block.successors[e];
        pred_edges[f][edge.target].emplace_back(b, e);
        cfg_succ[b].push_back(edge.target);
        cfg_pred[edge.target].push_back(b);
        for (const auto &phi : edge.phi)
          phi_defs[phi.first].emplace_back(f, std::make_pair(b, e));
      }
    }
    in_loop_block[f].assign(func.blocks.size(), false);
    if (!func.blocks.empty()) {
      std::vector<Id> comp;
      Id comp_count = 0;
      computeKosarajuSCCs(func.blocks.size(), cfg_succ, cfg_pred, comp,
                          comp_count);
      std::vector<std::size_t> comp_size(comp_count, 0);
      for (Id b = 0; b < func.blocks.size(); ++b)
        ++comp_size[comp[b]];
      for (Id b = 0; b < func.blocks.size(); ++b) {
        if (comp_size[comp[b]] > 1) {
          in_loop_block[f][b] = true;
        } else {
          for (Id succ : cfg_succ[b])
            if (succ == b)
              in_loop_block[f][b] = true;
        }
      }
    }
  }
}

void ProgramIndex::populateCallers(const Program &program, const Sets &upper) {
  for (auto &list : incoming_calls)
    list.clear();
  for (Id site : call_sites) {
    const Instruction &inst = *by_site[site].inst;
    const Targets targets =
        callees(program, inst, valueOf(program, upper, inst.indirect_target));
    for (Id callee : targets.functions)
      incoming_calls[callee].push_back(site);
  }
}

CallGraphInfo buildCallGraph(const Program &program, const Sets &upper) {
  CallGraphInfo info;
  const std::size_t n = program.functions.size();
  info.successors.resize(n);
  info.predecessors.resize(n);
  forInstructions(program, [&](Id caller, const Instruction &instruction) {
    if (instruction.opcode != Opcode::Call)
      return;
    const Targets targets =
        callees(program, instruction,
                valueOf(program, upper, instruction.indirect_target));
    for (Id callee : targets.functions) {
      if (std::find(info.successors[caller].begin(),
                    info.successors[caller].end(),
                    callee) == info.successors[caller].end()) {
        info.successors[caller].push_back(callee);
        info.predecessors[callee].push_back(caller);
      }
    }
  });

  Id sccCount = 0;
  computeKosarajuSCCs(n, info.successors, info.predecessors, info.function_scc,
                      sccCount);
  info.scc_count = sccCount;
  info.scc_members.resize(sccCount);
  info.scc_recursive.assign(sccCount, false);
  std::vector<std::vector<Id>> sccSuccessors(sccCount);
  std::vector<std::vector<Id>> sccPredecessors(sccCount);

  for (Id caller = 0; caller < n; ++caller) {
    const Id from = info.function_scc[caller];
    info.scc_members[from].push_back(caller);
    for (Id callee : info.successors[caller]) {
      const Id to = info.function_scc[callee];
      if (from == to) {
        info.scc_recursive[from] = true;
      } else if (std::find(sccSuccessors[from].begin(),
                           sccSuccessors[from].end(),
                           to) == sccSuccessors[from].end()) {
        sccSuccessors[from].push_back(to);
        sccPredecessors[to].push_back(from);
      }
    }
  }
  for (Id scc = 0; scc < sccCount; ++scc) {
    if (info.scc_members[scc].size() > 1)
      info.scc_recursive[scc] = true;
    if (info.scc_recursive[scc])
      ++info.recursive_scc_count;
  }

  std::vector<std::size_t> indegree(sccCount, 0);
  for (Id scc = 0; scc < sccCount; ++scc)
    indegree[scc] = sccPredecessors[scc].size();
  std::deque<Id> work;
  std::vector<std::size_t> rank(sccCount, 0);
  std::vector<Id> topo;
  topo.reserve(sccCount);
  for (Id scc = 0; scc < sccCount; ++scc)
    if (indegree[scc] == 0)
      work.push_back(scc);
  while (!work.empty()) {
    const Id scc = work.front();
    work.pop_front();
    topo.push_back(scc);
    for (Id successor : sccSuccessors[scc]) {
      rank[successor] = std::max(rank[successor], rank[scc] + 1);
      if (--indegree[successor] == 0)
        work.push_back(successor);
    }
  }
  info.reverse_topo_sccs.assign(topo.rbegin(), topo.rend());
  info.function_rank.resize(n);
  for (Id f = 0; f < n; ++f)
    info.function_rank[f] = rank[info.function_scc[f]];
  return info;
}

ReachabilityInfo computeReachability(const Program &program,
                                     const ProgramIndex &index,
                                     const Sets &upper) {
  ReachabilityInfo info;
  const std::size_t nf = program.functions.size();
  info.may_return.assign(nf, false);
  for (Id f = 0; f < nf; ++f)
    if (program.functions[f].external)
      info.may_return[f] = true;

  auto callMayReturn = [&](const Instruction &inst) -> bool {
    const Targets targets =
        callees(program, inst, valueOf(program, upper, inst.indirect_target));
    if (targets.opaque)
      return true;
    for (Id callee : targets.functions)
      if (info.may_return[callee])
        return true;
    return false;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (Id f = 0; f < nf; ++f) {
      if (info.may_return[f] || program.functions[f].external)
        continue;
      const Function &func = program.functions[f];
      std::vector<bool> visited(func.blocks.size(), false);
      std::deque<Id> q;
      q.push_back(0);
      visited[0] = true;
      bool returns = false;
      while (!q.empty() && !returns) {
        const Id b = q.front();
        q.pop_front();
        bool falls_through = true;
        for (const Instruction &inst : func.blocks[b].instructions) {
          if (inst.opcode == Opcode::Return) {
            returns = true;
            falls_through = false;
            break;
          }
          if (inst.opcode == Opcode::Call && !callMayReturn(inst)) {
            falls_through = false;
            break;
          }
        }
        if (falls_through) {
          for (const Edge &edge : func.blocks[b].successors) {
            if (!visited[edge.target]) {
              visited[edge.target] = true;
              q.push_back(edge.target);
            }
          }
        }
      }
      if (returns) {
        info.may_return[f] = true;
        changed = true;
      }
    }
  }

  info.reachable_site.assign(index.by_site.size(), false);
  for (Id f = 0; f < nf; ++f) {
    const Function &func = program.functions[f];
    if (func.external || func.blocks.empty())
      continue;
    std::vector<bool> visited(func.blocks.size(), false);
    std::deque<Id> q;
    q.push_back(0);
    visited[0] = true;
    while (!q.empty()) {
      const Id b = q.front();
      q.pop_front();
      bool falls_through = true;
      for (const Instruction &inst : func.blocks[b].instructions) {
        info.reachable_site[inst.id] = true;
        if (inst.opcode == Opcode::Return) {
          falls_through = false;
          break;
        }
        if (inst.opcode == Opcode::Call && !callMayReturn(inst)) {
          falls_through = false;
          break;
        }
      }
      if (falls_through) {
        for (const Edge &edge : func.blocks[b].successors) {
          if (!visited[edge.target]) {
            visited[edge.target] = true;
            q.push_back(edge.target);
          }
        }
      }
    }
  }

  info.reachable_function.assign(nf, false);
  if (program.entry < nf) {
    std::deque<Id> q{program.entry};
    info.reachable_function[program.entry] = true;
    while (!q.empty()) {
      const Id f = q.front();
      q.pop_front();
      const Function &func = program.functions[f];
      for (const Block &block : func.blocks) {
        for (const Instruction &inst : block.instructions) {
          if (inst.opcode != Opcode::Call || !info.reachable_site[inst.id])
            continue;
          const Targets targets = callees(
              program, inst, valueOf(program, upper, inst.indirect_target));
          for (Id callee : targets.functions) {
            if (!info.reachable_function[callee]) {
              info.reachable_function[callee] = true;
              q.push_back(callee);
            }
          }
        }
      }
    }
  }
  return info;
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
