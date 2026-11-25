#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"

#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Sparse St_P-restricted Andersen inclusion analysis (§2.1, Theorem 7).
Sets inclusion(const Program &program, const ProgramIndex &index,
               const Slice &slice) {
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
  auto store_one = [&](Id o, const PointsToSet &rhs, std::uint64_t bytes) {
    if (o < 2 || o >= slice.objects.size() || !slice.objects[o] ||
        !isMemory(program.objects[o]))
      return;
    const Object &info = program.objects[o];
    const bool incompatible =
        bytes != 0 && (info.bytes == 0 || info.bytes != bytes);
    changed |= memory[o].join(incompatible ? PointsToSet::top() : rhs);
  };
  auto store = [&](const PointsToSet &address, const PointsToSet &rhs,
                   std::uint64_t bytes = 0) {
    if (address.empty())
      return;
    if (address.isTop()) {
      for (Id o : slice.object_list)
        store_one(o, rhs, bytes);
    } else {
      for (Id o : address.objects()) {
        store_one(o, rhs, bytes);
        if (bytes != 0 && isMemory(program.objects[o])) {
          for (Id fo : program.overlappingFields(o, bytes))
            if (fo != o)
              store_one(fo, PointsToSet::top(), 0);
        }
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
              if (!isMemory(info) ||
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
                changed |= memory[fd].join(PointsToSet::top());
                continue;
              }
              for (Id os : src_pts.objects()) {
                if (!isMemory(program.objects[os]) ||
                    (os < index.has_unaligned_interior.size() &&
                     index.has_unaligned_interior[os])) {
                  changed |= memory[fd].join(PointsToSet::top());
                  continue;
                }
                for (std::int64_t rel : rels) {
                  const PointsToSet fs_pts =
                      program.resolveField(os, rel, true);
                  if (fs_pts.isTop() || fs_pts.empty()) {
                    changed |= memory[fd].join(PointsToSet::top());
                    continue;
                  }
                  for (Id fs : fs_pts.objects()) {
                    const Object &fs_info = program.objects[fs];
                    if (!isMemory(fs_info) || fs_info.bytes != fd_info.bytes ||
                        !slice.objects[fs])
                      changed |= memory[fd].join(PointsToSet::top());
                    else
                      changed |= memory[fd].join(memory[fs]);
                  }
                }
              }
            }
          }
        }
        break;
      }
      case Opcode::Havoc:
        rhs = PointsToSet::top();
        if (i.operands.empty()) {
          store(PointsToSet::top(), PointsToSet::top());
        } else {
          const PointsToSet addr = eval(i.operands[0]);
          if (addr.isTop()) {
            store(PointsToSet::top(), PointsToSet::top());
          } else {
            for (Id o : addr.objects()) {
              if (!isMemory(program.objects[o]))
                continue;
              for (Id fo : program.overlappingFields(o, i.write_bytes))
                store_one(fo, PointsToSet::top(), 0);
            }
          }
        }
        break;
      case Opcode::Return:
        if (!i.operands.empty())
          changed |= returns[f].join(eval(i.operands[0]));
        break;
      case Opcode::Call: {
        const Targets targets = callees(program, i, eval(i.indirect_target));
        if (targets.opaque) {
          rhs = PointsToSet::top();
          if (i.writes_memory)
            store(PointsToSet::top(), PointsToSet::top());
        }
        for (Id callee_id : targets.functions) {
          const Function &callee = program.functions[callee_id];
          if (callee.external) {
            rhs = PointsToSet::top();
            if (i.writes_memory && callee.writes_memory)
              store(PointsToSet::top(), PointsToSet::top());
          } else {
            for (std::size_t a = 0; a < callee.parameters.size(); ++a) {
              const Id parameter = callee.parameters[a];
              if (parameter != INVALID && slice.values[parameter])
                changed |= values[parameter].join(
                    eval(a < i.operands.size() ? i.operands[a] : INVALID));
            }
            rhs.join(returns[callee_id]);
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
