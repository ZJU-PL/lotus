#include "Alias/InclusionBased/BootstrapAA/Preprocessing/RelevantSlice.h"

#include <deque>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

bool touches(const Program & /*program*/, const Slice &slice,
             const PointsToSet &targets) {
  if (targets.empty())
    return false;
  if (targets.isTop())
    return !slice.object_list.empty();
  for (Id object : targets.objects()) {
    if (object >= 2 && object < slice.objects.size() && slice.objects[object])
      return true;
  }
  return false;
}

// Algorithm 1 (Kahlon PLDI '08, p. 253): worklist-based backward closure
// computing both V_P (relevant values/objects) and St_P (relevant statements).
Slice relevant(const Program &program, const ProgramIndex &index,
               const Sets &upper, const SteensgaardHierarchy &hierarchy,
               const ReachabilityInfo &reachability,
               const std::vector<Id> &seeds, bool enabled) {
  Slice result;
  result.values.assign(program.values.size(), !enabled);
  result.objects.assign(program.objects.size(), !enabled);
  result.instructions.assign(index.by_site.size(), !enabled);
  result.relevant_functions.assign(program.functions.size(), !enabled);

  if (!enabled) {
    for (Id v = 0; v < program.values.size(); ++v)
      result.value_list.push_back(v);
    for (Id o = 2; o < program.objects.size(); ++o)
      if (isMemory(program.objects[o]))
        result.object_list.push_back(o);
      else
        result.objects[o] = false;
    for (Id s = 0; s < index.by_site.size(); ++s)
      if (index.by_site[s].inst)
        result.inst_list.push_back(s);
    return result;
  }

  std::vector<bool> relevantHigher(hierarchy.successors.size(), false);
  std::deque<Id> value_work;
  std::deque<Id> object_work;
  std::deque<Id> higher_work;

  // Map hierarchy component -> store/havoc/memcopy sites whose address is in
  // that component, plus top-address sites.
  std::vector<std::vector<Id>> stores_by_comp(hierarchy.successors.size());
  std::vector<Id> top_stores;
  for (Id site : index.store_sites) {
    const Instruction &i = *index.by_site[site].inst;
    const Id addr = i.operands[0];
    if (addr == INVALID || valueOf(program, upper, addr).isTop())
      top_stores.push_back(site);
    else
      stores_by_comp[hierarchy.value_components[addr]].push_back(site);
  }
  std::vector<std::vector<Id>> havocs_by_comp(hierarchy.successors.size());
  std::vector<std::vector<Id>> havocs_by_obj(program.objects.size());
  std::vector<Id> top_havocs;
  for (Id site : index.havoc_sites) {
    const Instruction &i = *index.by_site[site].inst;
    if (i.operands.empty() || i.operands[0] == INVALID ||
        valueOf(program, upper, i.operands[0]).isTop()) {
      top_havocs.push_back(site);
    } else {
      havocs_by_comp[hierarchy.value_components[i.operands[0]]].push_back(site);
      const PointsToSet pts = valueOf(program, upper, i.operands[0]);
      for (Id o : pts.objects()) {
        if (!isMemory(program.objects[o]))
          continue;
        for (Id fo : program.overlappingFields(o, i.write_bytes))
          if (isMemory(program.objects[fo]))
            havocs_by_obj[fo].push_back(site);
      }
    }
  }
  std::vector<std::vector<Id>> memcopies_by_comp(hierarchy.successors.size());
  std::vector<std::vector<Id>> memcopies_by_obj(program.objects.size());
  std::vector<Id> top_memcopies;
  for (Id site : index.memcopy_sites) {
    const Instruction &i = *index.by_site[site].inst;
    const Id dst = i.operands[0];
    if (dst == INVALID || valueOf(program, upper, dst).isTop()) {
      top_memcopies.push_back(site);
    } else {
      memcopies_by_comp[hierarchy.value_components[dst]].push_back(site);
      const PointsToSet pts = valueOf(program, upper, dst);
      for (Id o : pts.objects()) {
        if (!isMemory(program.objects[o]))
          continue;
        for (Id fo : program.overlappingFields(o, i.write_bytes))
          if (isMemory(program.objects[fo]))
            memcopies_by_obj[fo].push_back(site);
      }
    }
  }
  std::vector<std::vector<Id>> allocs_by_base(program.objects.size());
  for (Id site : index.allocate_sites) {
    const Instruction &i = *index.by_site[site].inst;
    if (i.object < program.objects.size())
      allocs_by_base[program.baseOf(i.object)].push_back(site);
  }

  auto markInst = [&](Id site) {
    if (site < result.instructions.size() && !result.instructions[site]) {
      result.instructions[site] = true;
      result.inst_list.push_back(site);
      result.relevant_functions[index.by_site[site].function] = true;
    }
  };

  auto markHigherComponent = [&](Id comp) {
    std::vector<Id> stack(hierarchy.predecessors[comp].begin(),
                          hierarchy.predecessors[comp].end());
    if (hierarchy.cyclic[comp])
      stack.push_back(comp);
    while (!stack.empty()) {
      const Id current = stack.back();
      stack.pop_back();
      if (relevantHigher[current])
        continue;
      relevantHigher[current] = true;
      higher_work.push_back(current);
      stack.insert(stack.end(), hierarchy.predecessors[current].begin(),
                   hierarchy.predecessors[current].end());
    }
  };

  auto needValue = [&](Id value) {
    if (value == INVALID || result.values[value])
      return;
    result.values[value] = true;
    result.value_list.push_back(value);
    value_work.push_back(value);
    markHigherComponent(hierarchy.value_components[value]);
  };

  bool activated_top_memory_ops = false;
  auto needObject = [&](Id object) {
    if (object < 2 || object >= program.objects.size() ||
        !isMemory(program.objects[object]) || result.objects[object])
      return;
    result.objects[object] = true;
    result.object_list.push_back(object);
    object_work.push_back(object);
    const Id ocomp = hierarchy.object_components[object];
    if (!relevantHigher[ocomp]) {
      relevantHigher[ocomp] = true;
      higher_work.push_back(ocomp);
    }
    for (Id alloc_site : allocs_by_base[program.baseOf(object)])
      markInst(alloc_site);
  };

  auto needMemory = [&](const PointsToSet &set) {
    if (set.empty())
      return;
    if (set.isTop()) {
      for (Id o = 2; o < program.objects.size(); ++o)
        if (isMemory(program.objects[o]))
          needObject(o);
      return;
    }
    for (Id o : set.objects())
      needObject(o);
  };

  auto processStore = [&](Id site) {
    if (result.instructions[site])
      return;
    const Instruction &i = *index.by_site[site].inst;
    markInst(site);
    needValue(i.operands[0]);
    needValue(i.operands[1]);
    needMemory(valueOf(program, upper, i.operands[0]));
  };

  auto processHavoc = [&](Id site) {
    if (result.instructions[site])
      return;
    const Instruction &i = *index.by_site[site].inst;
    markInst(site);
    if (!i.operands.empty()) {
      needValue(i.operands[0]);
      const PointsToSet pts = valueOf(program, upper, i.operands[0]);
      if (pts.isTop()) {
        needMemory(pts);
      } else {
        for (Id o : pts.objects()) {
          if (!isMemory(program.objects[o]))
            continue;
          for (Id fo : program.overlappingFields(o, i.write_bytes))
            needObject(fo);
        }
      }
    }
  };

  auto processMemCopy = [&](Id site) {
    if (result.instructions[site])
      return;
    const Instruction &i = *index.by_site[site].inst;
    markInst(site);
    needValue(i.operands[0]);
    needValue(i.operands[1]);
    const PointsToSet dst_pts = valueOf(program, upper, i.operands[0]);
    const PointsToSet src_pts = valueOf(program, upper, i.operands[1]);
    if (dst_pts.isTop()) {
      needMemory(dst_pts);
    } else {
      for (Id od : dst_pts.objects())
        for (Id fd : program.overlappingFields(od, i.write_bytes))
          needObject(fd);
    }
    if (src_pts.isTop()) {
      needMemory(src_pts);
    } else {
      for (Id os : src_pts.objects())
        for (Id fs : program.overlappingFields(os, i.write_bytes))
          needObject(fs);
    }
  };

  std::vector<Id> memory_writing_calls;
  std::vector<Id> non_returning_calls;
  for (Id site : index.call_sites) {
    const Instruction &i = *index.by_site[site].inst;
    const Targets targets =
        callees(program, i, valueOf(program, upper, i.indirect_target));
    bool writes_ext = targets.opaque && i.writes_memory;
    bool has_non_returning = false;
    for (Id callee : targets.functions) {
      if (program.functions[callee].external && i.writes_memory &&
          program.functions[callee].writes_memory)
        writes_ext = true;
      if (!reachability.may_return[callee])
        has_non_returning = true;
    }
    if (writes_ext)
      memory_writing_calls.push_back(site);
    if (has_non_returning)
      non_returning_calls.push_back(site);
  }
  for (Id site : non_returning_calls) {
    const Instruction &i = *index.by_site[site].inst;
    if (i.indirect_target != INVALID) {
      markInst(site);
      needValue(i.indirect_target);
    }
  }

  for (Id v : seeds)
    needValue(v);

  std::vector<bool> fn_relevant_propagated(program.functions.size(), false);
  std::deque<Id> fn_work;
  auto markFuncRelevant = [&](Id f) {
    if (f < program.functions.size() && !fn_relevant_propagated[f]) {
      fn_relevant_propagated[f] = true;
      result.relevant_functions[f] = true;
      fn_work.push_back(f);
    }
  };

  while (!value_work.empty() || !object_work.empty() || !higher_work.empty() ||
         !fn_work.empty()) {
    while (!value_work.empty()) {
      const Id v = value_work.front();
      value_work.pop_front();

      for (Id site : index.defining_sites[v]) {
        const Instruction &i = *index.by_site[site].inst;
        markInst(site);
        markFuncRelevant(index.by_site[site].function);
        switch (i.opcode) {
        case Opcode::Allocate:
        case Opcode::Copy:
        case Opcode::Join:
        case Opcode::Field:
          for (Id op : i.operands)
            needValue(op);
          break;
        case Opcode::Load:
          needValue(i.operands[0]);
          needMemory(valueOf(program, upper, i.operands[0]));
          break;
        case Opcode::Havoc:
          if (!i.operands.empty()) {
            needValue(i.operands[0]);
            needMemory(valueOf(program, upper, i.operands[0]));
          }
          break;
        case Opcode::Call: {
          needValue(i.indirect_target);
          const Targets targets =
              callees(program, i, valueOf(program, upper, i.indirect_target));
          for (Id f : targets.functions) {
            markFuncRelevant(f);
            for (Id ret_site : index.return_sites[f]) {
              const Instruction &ret = *index.by_site[ret_site].inst;
              markInst(ret_site);
              if (!ret.operands.empty())
                needValue(ret.operands[0]);
            }
          }
          break;
        }
        default:
          break;
        }
      }

      for (const auto &phi_loc : index.phi_defs[v]) {
        markFuncRelevant(phi_loc.first);
        const Edge &edge = program.functions[phi_loc.first]
                               .blocks[phi_loc.second.first]
                               .successors[phi_loc.second.second];
        for (auto phi : edge.phi)
          if (phi.first == v)
            needValue(phi.second);
      }

      const Id owner = program.values[v].function;
      if (owner != INVALID) {
        const auto &params = program.functions[owner].parameters;
        for (std::size_t a = 0; a < params.size(); ++a) {
          if (params[a] != v)
            continue;
          markFuncRelevant(owner);
          for (Id call_site : index.incoming_calls[owner]) {
            const Instruction &ci = *index.by_site[call_site].inst;
            markInst(call_site);
            markFuncRelevant(index.by_site[call_site].function);
            needValue(ci.indirect_target);
            if (a < ci.operands.size())
              needValue(ci.operands[a]);
          }
        }
      }
    }

    while (!higher_work.empty()) {
      const Id comp = higher_work.front();
      higher_work.pop_front();
      for (Id site : stores_by_comp[comp]) {
        processStore(site);
        markFuncRelevant(index.by_site[site].function);
      }
      for (Id site : havocs_by_comp[comp]) {
        processHavoc(site);
        markFuncRelevant(index.by_site[site].function);
      }
      for (Id site : memcopies_by_comp[comp]) {
        processMemCopy(site);
        markFuncRelevant(index.by_site[site].function);
      }
    }

    while (!object_work.empty()) {
      const Id obj = object_work.front();
      object_work.pop_front();
      if (!activated_top_memory_ops) {
        activated_top_memory_ops = true;
        for (Id site : top_stores) {
          processStore(site);
          markFuncRelevant(index.by_site[site].function);
        }
        for (Id site : top_havocs) {
          processHavoc(site);
          markFuncRelevant(index.by_site[site].function);
        }
        for (Id site : top_memcopies) {
          processMemCopy(site);
          markFuncRelevant(index.by_site[site].function);
        }
        for (Id site : memory_writing_calls) {
          const Instruction &ci = *index.by_site[site].inst;
          markInst(site);
          markFuncRelevant(index.by_site[site].function);
          needValue(ci.indirect_target);
        }
      }
      for (Id site : havocs_by_obj[obj]) {
        processHavoc(site);
        markFuncRelevant(index.by_site[site].function);
      }
      for (Id site : memcopies_by_obj[obj]) {
        processMemCopy(site);
        markFuncRelevant(index.by_site[site].function);
      }
      const Id base = program.baseOf(obj);
      if (base != obj) {
        const Id bcomp = hierarchy.object_components[base];
        if (!relevantHigher[bcomp]) {
          relevantHigher[bcomp] = true;
          higher_work.push_back(bcomp);
        }
      }
    }

    while (!fn_work.empty()) {
      const Id f = fn_work.front();
      fn_work.pop_front();
      for (Id call_site : index.incoming_calls[f]) {
        const Instruction &ci = *index.by_site[call_site].inst;
        markInst(call_site);
        if (ci.indirect_target != INVALID)
          needValue(ci.indirect_target);
        markFuncRelevant(index.by_site[call_site].function);
      }
    }
  }

  return result;
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
