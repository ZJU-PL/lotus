#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"

#include <deque>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Optional Das (2000) One-Level Flow intermediate cascade stage (paper §4
// p. 258): directional inclusion on top-level assignments
// (Copy/Join/Field/PHI/Call) while reusing Steensgaard's unified memory
// targets for Load/Store.
Sets oneLevelFlow(const Program &program, const ProgramIndex &index,
                  const std::vector<Id> &partition, const Sets &coarse_upper) {
  Sets values(program.values.size());
  std::vector<bool> active(program.values.size(), false);
  std::deque<Id> seed_work(partition.begin(), partition.end());
  for (Id v : partition)
    active[v] = true;
  auto activate = [&](Id v) {
    if (v != INVALID && !active[v]) {
      active[v] = true;
      seed_work.push_back(v);
    }
  };
  while (!seed_work.empty()) {
    const Id v = seed_work.front();
    seed_work.pop_front();
    for (Id site : index.defining_sites[v]) {
      const Instruction &i = *index.by_site[site].inst;
      if (i.opcode == Opcode::Copy || i.opcode == Opcode::Join ||
          i.opcode == Opcode::Field || i.opcode == Opcode::Allocate) {
        for (Id op : i.operands)
          activate(op);
      } else if (i.opcode == Opcode::Call) {
        const Targets targets = callees(
            program, i, valueOf(program, coarse_upper, i.indirect_target));
        for (Id f : targets.functions)
          for (Id ret_site : index.return_sites[f]) {
            const Instruction &ret = *index.by_site[ret_site].inst;
            if (!ret.operands.empty())
              activate(ret.operands[0]);
          }
      }
    }
    for (const auto &phi_loc : index.phi_defs[v]) {
      const Edge &edge = program.functions[phi_loc.first]
                             .blocks[phi_loc.second.first]
                             .successors[phi_loc.second.second];
      for (auto p : edge.phi)
        if (p.first == v)
          activate(p.second);
    }
    const Id owner = program.values[v].function;
    if (owner != INVALID) {
      const auto &params = program.functions[owner].parameters;
      for (std::size_t a = 0; a < params.size(); ++a) {
        if (params[a] != v)
          continue;
        for (Id call_site : index.incoming_calls[owner]) {
          const Instruction &ci = *index.by_site[call_site].inst;
          if (a < ci.operands.size())
            activate(ci.operands[a]);
        }
      }
    }
  }

  for (Id v = 0; v < program.values.size(); ++v)
    if (active[v] && program.values[v].constant)
      values[v] = program.values[v].initial;
  for (Id p : program.functions[program.entry].parameters)
    if (p != INVALID && active[p])
      values[p] = PointsToSet::top();

  std::vector<Id> active_list;
  for (Id v = 0; v < program.values.size(); ++v)
    if (active[v])
      active_list.push_back(v);

  bool changed = true;
  while (changed) {
    changed = false;
    for (Id v : active_list) {
      for (Id site : index.defining_sites[v]) {
        const Instruction &i = *index.by_site[site].inst;
        switch (i.opcode) {
        case Opcode::Address:
        case Opcode::Allocate:
          changed |= values[v].insert(i.object);
          if (i.may_be_null)
            changed |= values[v].insert(NULL_OBJECT);
          for (Id op : i.operands)
            changed |= values[v].join(valueOf(program, values, op));
          break;
        case Opcode::Unknown:
        case Opcode::Havoc:
          changed |= values[v].join(PointsToSet::top());
          break;
        case Opcode::Copy:
        case Opcode::Join:
          for (Id op : i.operands)
            changed |= values[v].join(valueOf(program, values, op));
          break;
        case Opcode::Field: {
          const PointsToSet base_pts = valueOf(program, values, i.operands[0]);
          if (base_pts.isTop()) {
            changed |= values[v].join(PointsToSet::top());
          } else {
            for (Id obj : base_pts.objects())
              changed |= values[v].join(program.resolveField(
                  obj, i.field_offset, i.field_known, i.field_array_stride));
          }
          break;
        }
        case Opcode::Load:
          // One-level flow reuses Steensgaard's unified memory abstraction.
          changed |= values[v].join(coarse_upper[v]);
          break;
        case Opcode::Call: {
          const Targets targets = callees(
              program, i, valueOf(program, coarse_upper, i.indirect_target));
          if (targets.opaque)
            changed |= values[v].join(PointsToSet::top());
          for (Id f : targets.functions) {
            if (program.functions[f].external) {
              changed |= values[v].join(PointsToSet::top());
            } else {
              for (Id ret_site : index.return_sites[f]) {
                const Instruction &ret = *index.by_site[ret_site].inst;
                if (!ret.operands.empty())
                  changed |=
                      values[v].join(valueOf(program, values, ret.operands[0]));
              }
            }
          }
          break;
        }
        default:
          break;
        }
      }
      for (const auto &phi_loc : index.phi_defs[v]) {
        const Edge &edge = program.functions[phi_loc.first]
                               .blocks[phi_loc.second.first]
                               .successors[phi_loc.second.second];
        for (auto p : edge.phi)
          if (p.first == v)
            changed |= values[v].join(valueOf(program, values, p.second));
      }
      const Id owner = program.values[v].function;
      if (owner != INVALID) {
        const auto &params = program.functions[owner].parameters;
        for (std::size_t a = 0; a < params.size(); ++a) {
          if (params[a] != v)
            continue;
          for (Id call_site : index.incoming_calls[owner]) {
            const Instruction &ci = *index.by_site[call_site].inst;
            const Id actual = a < ci.operands.size() ? ci.operands[a] : INVALID;
            changed |= values[v].join(valueOf(program, values, actual));
          }
        }
      }
    }
  }
  return values;
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
