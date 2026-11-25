#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"

#include <map>
#include <set>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Sparse St_P-restricted Andersen inclusion analysis (§2.1, Theorem 7).
Sets inclusion(const Program &program, const ProgramIndex &index,
               const Slice &slice, HavocPolicy havoc_policy) {
  const bool noop_havoc = (havoc_policy == HavocPolicy::Noop);
  Sets values(program.values.size()), memory(program.objects.size());
  Sets returns(program.functions.size());
  for (Id o : slice.object_list)
    memory[o] = program.objects[o].initial;
  for (Id parameter : program.functions[program.entry].parameters)
    if (parameter != INVALID && slice.values[parameter])
      values[parameter] = PointsToSet::top();

  struct PhiEdge {
    Id dst;
    Id src;
  };
  std::vector<PhiEdge> relevant_phis;
  for (Id v : slice.value_list) {
    for (const auto &phi_loc : index.phi_defs[v]) {
      const Edge &edge = program.functions[phi_loc.first]
                             .blocks[phi_loc.second.first]
                             .successors[phi_loc.second.second];
      for (auto p : edge.phi)
        if (p.first == v)
          relevant_phis.push_back({p.first, p.second});
    }
  }

  bool changed = false;
  auto eval = [&](Id value) { return valueOf(program, values, value); };
  std::vector<Id> addr_taken_funcs;
  for (Id f = 0; f < program.functions.size(); ++f)
    if (program.functions[f].address_taken)
      addr_taken_funcs.push_back(f);

  std::map<std::uint64_t, PointsToSet> top_store_accum;
  std::map<std::uint64_t, PointsToSet> top_store_flushed;
  std::set<std::uint64_t> top_store_incompat_done;

  auto store_one = [&](Id o, const PointsToSet &rhs, std::uint64_t bytes) {
    if (o < 2 || o >= slice.objects.size() || !slice.objects[o] ||
        !isMemory(program.objects[o]))
      return;
    const Object &info = program.objects[o];
    const bool incompatible =
        bytes != 0 && (info.bytes == 0 || info.bytes != bytes);
    if (incompatible) {
      if (!noop_havoc)
        changed |= memory[o].join(PointsToSet::top());
    } else {
      changed |= memory[o].join(rhs);
    }
  };
  auto store = [&](const PointsToSet &address, const PointsToSet &rhs,
                   std::uint64_t bytes = 0) {
    if (address.empty())
      return;
    if (address.isTop()) {
      if (!noop_havoc && bytes != 0 &&
          top_store_incompat_done.insert(bytes).second) {
        for (Id o : slice.object_list) {
          if (o < 2 || !isMemory(program.objects[o]))
            continue;
          const Object &info = program.objects[o];
          if (info.bytes == 0 || info.bytes != bytes)
            changed |= memory[o].join(PointsToSet::top());
        }
      }
      if (!rhs.empty())
        top_store_accum[bytes].join(rhs);
    } else {
      if (noop_havoc && rhs.empty())
        return;
      for (Id o : address.objects()) {
        store_one(o, rhs, bytes);
        if (!noop_havoc && bytes != 0 && isMemory(program.objects[o])) {
          for (Id fo : program.overlappingFields(o, bytes))
            if (fo != o)
              store_one(fo, PointsToSet::top(), 0);
        }
      }
    }
  };
  auto flushTopStores = [&]() {
    for (const auto &[bytes, accum] : top_store_accum) {
      PointsToSet &flushed = top_store_flushed[bytes];
      if (!flushed.join(accum))
        continue;
      for (Id o : slice.object_list) {
        if (o < 2 || !isMemory(program.objects[o]))
          continue;
        const Object &info = program.objects[o];
        if (bytes == 0 || (info.bytes != 0 && info.bytes == bytes))
          changed |= memory[o].join(flushed);
      }
    }
  };

  do {
    changed = false;
    for (Id site : slice.inst_list) {
      const InstLocation &loc = index.by_site[site];
      const Id f = loc.function;
      const Instruction &i = *loc.inst;
      const bool defines = i.result != INVALID && slice.values[i.result];
      PointsToSet rhs;
      switch (i.opcode) {
      case Opcode::Address:
      case Opcode::Allocate:
        if (defines) {
          rhs.insert(i.object);
          if (i.may_be_null)
            rhs.insert(NULL_OBJECT);
          for (Id operand : i.operands)
            rhs.join(eval(operand));
        }
        break;
      case Opcode::Unknown:
        rhs = PointsToSet::top();
        break;
      case Opcode::Copy:
      case Opcode::Join:
        if (defines)
          for (Id operand : i.operands)
            rhs.join(eval(operand));
        break;
      case Opcode::Field:
        if (defines) {
          const PointsToSet base_pts = eval(i.operands[0]);
          if (base_pts.isTop()) {
            rhs = PointsToSet::top();
          } else {
            for (Id obj : base_pts.objects())
              rhs.join(program.resolveField(obj, i.field_offset, i.field_known,
                                            i.field_array_stride));
          }
        }
        break;
      case Opcode::Load:
        if (defines) {
          const PointsToSet address = eval(i.operands[0]);
          if (address.isTop()) {
            rhs = PointsToSet::top();
          } else {
            for (Id object : address.objects()) {
              if (object == NULL_OBJECT)
                continue;
              const Object &info = program.objects[object];
              if (!isMemory(info) || object >= slice.objects.size() ||
                  !slice.objects[object] ||
                  (i.read_bytes != 0 &&
                   (info.bytes == 0 || info.bytes != i.read_bytes)))
                rhs.join(PointsToSet::top());
              else
                rhs.join(memory[object]);
            }
          }
        }
        break;
      case Opcode::Store:
        store(eval(i.operands[0]), eval(i.operands[1]), i.write_bytes);
        break;
      case Opcode::MemCopy: {
        const PointsToSet dst_pts = eval(i.operands[0]);
        const PointsToSet src_pts = eval(i.operands[1]);
        if (dst_pts.isTop()) {
          if (!noop_havoc)
            store(PointsToSet::top(), PointsToSet::top());
        } else {
          for (Id od : dst_pts.objects()) {
            if (!isMemory(program.objects[od]))
              continue;
            for (Id fd : program.overlappingFields(od, i.write_bytes)) {
              if (!slice.objects[fd])
                continue;
              const Object &fd_info = program.objects[fd];
              std::vector<std::int64_t> rels;
              const bool dst_unaligned =
                  od < index.has_unaligned_interior.size() &&
                  index.has_unaligned_interior[od];
              if (src_pts.isTop() || dst_unaligned ||
                  !program.memCopyRelativeOffsets(od, fd, i.write_bytes,
                                                  rels)) {
                if (!noop_havoc)
                  changed |= memory[fd].join(PointsToSet::top());
                continue;
              }
              for (Id os : src_pts.objects()) {
                if (os == NULL_OBJECT)
                  continue;
                if (!isMemory(program.objects[os]) ||
                    (os < index.has_unaligned_interior.size() &&
                     index.has_unaligned_interior[os])) {
                  if (!noop_havoc)
                    changed |= memory[fd].join(PointsToSet::top());
                  continue;
                }
                for (std::int64_t rel : rels) {
                  const PointsToSet fs_pts =
                      program.resolveField(os, rel, true);
                  if (fs_pts.isTop() || fs_pts.empty()) {
                    if (!noop_havoc)
                      changed |= memory[fd].join(PointsToSet::top());
                    continue;
                  }
                  for (Id fs : fs_pts.objects()) {
                    const Object &fs_info = program.objects[fs];
                    if (!isMemory(fs_info) || fs_info.bytes != fd_info.bytes ||
                        !slice.objects[fs]) {
                      if (!noop_havoc)
                        changed |= memory[fd].join(PointsToSet::top());
                    } else {
                      changed |= memory[fd].join(memory[fs]);
                    }
                  }
                }
              }
            }
          }
        }
        break;
      }
      case Opcode::Havoc:
        if (!noop_havoc)
          rhs = PointsToSet::top();
        if (i.operands.empty()) {
          if (!noop_havoc)
            store(PointsToSet::top(), PointsToSet::top());
        } else {
          const PointsToSet addr = eval(i.operands[0]);
          if (addr.isTop()) {
            if (!noop_havoc)
              store(PointsToSet::top(), PointsToSet::top());
          } else {
            for (Id o : addr.objects()) {
              if (!isMemory(program.objects[o]))
                continue;
              const bool o_unaligned =
                  o < index.has_unaligned_interior.size() &&
                  index.has_unaligned_interior[o];
              for (Id fo : program.overlappingFields(o, i.write_bytes)) {
                if (i.zero_fill && !o_unaligned &&
                    program.zeroFillCoversField(o, fo, i.write_bytes))
                  store_one(fo, PointsToSet(NULL_OBJECT), 0);
                else if (!noop_havoc)
                  store_one(fo, PointsToSet::top(), 0);
              }
            }
          }
        }
        break;
      case Opcode::Return:
        if (!i.operands.empty())
          changed |= returns[f].join(eval(i.operands[0]));
        break;
      case Opcode::Call: {
        auto handleCallee = [&](Id callee_id) {
          const Function &callee = program.functions[callee_id];
          if (callee.external) {
            if (!noop_havoc) {
              rhs = PointsToSet::top();
              if (i.writes_memory && callee.writes_memory)
                store(PointsToSet::top(), PointsToSet::top());
            }
          } else {
            for (std::size_t a = 0; a < callee.parameters.size(); ++a) {
              const Id parameter = callee.parameters[a];
              if (parameter != INVALID && slice.values[parameter])
                changed |= values[parameter].join(
                    eval(a < i.operands.size() ? i.operands[a] : INVALID));
            }
            rhs.join(returns[callee_id]);
          }
        };
        if (i.callee != INVALID) {
          if (i.opaque_alternative && !noop_havoc) {
            rhs = PointsToSet::top();
            if (i.writes_memory)
              store(PointsToSet::top(), PointsToSet::top());
          }
          handleCallee(i.callee);
        } else {
          const PointsToSet tgt_pts = eval(i.indirect_target);
          if (tgt_pts.isTop()) {
            if (!noop_havoc) {
              rhs = PointsToSet::top();
              if (i.writes_memory)
                store(PointsToSet::top(), PointsToSet::top());
            }
            for (Id callee_id : addr_taken_funcs)
              handleCallee(callee_id);
          } else {
            const Targets targets = callees(program, i, tgt_pts);
            if (targets.opaque && !noop_havoc) {
              rhs = PointsToSet::top();
              if (i.writes_memory)
                store(PointsToSet::top(), PointsToSet::top());
            }
            for (Id callee_id : targets.functions)
              handleCallee(callee_id);
          }
        }
        break;
      }
      default:
        break;
      }
      if (defines)
        changed |= values[i.result].join(rhs);
    }
    flushTopStores();
    for (const PhiEdge &phi : relevant_phis)
      changed |= values[phi.dst].join(eval(phi.src));
  } while (changed);

  for (Id v = 0; v < program.values.size(); ++v)
    if (program.values[v].constant)
      values[v] = program.values[v].initial;
  return values;
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
