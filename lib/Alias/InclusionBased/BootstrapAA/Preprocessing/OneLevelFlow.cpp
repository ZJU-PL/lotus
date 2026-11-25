#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"

#include <deque>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Optional Stage 1.5 (paper §4, p. 258): Das (2000) One-Level Flow cascade.
// Performs directional inclusion across BOTH top-level assignments
// (Copy/Join/Field/PHI/Call) AND single-level memory transfers
// (Load/Store/MemCopy/Havoc) over the partition's slice, while fixing all
// dereference address targets and indirect call targets to Steensgaard's
// unified `coarse_upper` sets. Because every flow edge is static once
// `coarse_upper` is fixed, propagation runs via a fast worklist over the
// static 1-level flow graph.
Sets oneLevelFlow(const Program &program, const ProgramIndex &index,
                  const Slice &slice, const Sets &coarse_upper,
                  HavocPolicy havoc_policy) {
  const bool noop_havoc = (havoc_policy == HavocPolicy::Noop);
  const std::size_t nv = program.values.size();
  const std::size_t no = program.objects.size();
  const std::size_t total_nodes = nv + no;

  auto valNode = [](Id v) -> std::size_t { return v; };
  auto memNode = [nv](Id o) -> std::size_t { return nv + o; };

  struct FlowEdge {
    std::size_t dst = 0;
    bool is_field = false;
    std::int64_t field_offset = 0;
    bool field_known = true;
    std::uint64_t field_stride = 0;
  };

  std::vector<std::vector<FlowEdge>> adj(total_nodes);
  std::vector<PointsToSet> pts(total_nodes);
  std::vector<bool> in_queue(total_nodes, false);
  std::deque<std::size_t> work;

  auto enqueue = [&](std::size_t node) {
    if (!in_queue[node]) {
      in_queue[node] = true;
      work.push_back(node);
    }
  };
  auto seed = [&](std::size_t node, const PointsToSet &s) {
    if (pts[node].join(s))
      enqueue(node);
  };
  auto seedObj = [&](std::size_t node, Id obj) {
    if (pts[node].insert(obj))
      enqueue(node);
  };
  auto addCopyEdge = [&](std::size_t src, std::size_t dst) {
    if (src != dst)
      adj[src].push_back({dst, false, 0, true, 0});
  };
  auto addFieldEdge = [&](std::size_t src, std::size_t dst, std::int64_t off,
                          bool known, std::uint64_t stride) {
    adj[src].push_back({dst, true, off, known, stride});
  };
  auto addValSourceEdge = [&](Id src_val, std::size_t dst) {
    if (src_val == INVALID) {
      seed(dst, PointsToSet::top());
    } else if (program.values[src_val].constant) {
      seed(dst, program.values[src_val].initial);
    } else if (slice.values[src_val]) {
      addCopyEdge(valNode(src_val), dst);
    } else {
      seed(dst, valueOf(program, coarse_upper, src_val));
    }
  };

  for (Id v : slice.value_list) {
    if (program.values[v].constant)
      seed(valNode(v), program.values[v].initial);
  }
  for (Id p : program.functions[program.entry].parameters) {
    if (p != INVALID && slice.values[p])
      seed(valNode(p), PointsToSet::top());
  }
  for (Id o : slice.object_list)
    seed(memNode(o), program.objects[o].initial);

  auto havocAllSliceMem = [&]() {
    for (Id o : slice.object_list)
      seed(memNode(o), PointsToSet::top());
  };

  auto addStoreToObj = [&](Id o, Id rhs_val, std::uint64_t bytes) {
    if (o < 2 || o >= slice.objects.size() || !slice.objects[o] ||
        !isMemory(program.objects[o]))
      return;
    const Object &info = program.objects[o];
    const bool incompatible =
        bytes != 0 && (info.bytes == 0 || info.bytes != bytes);
    if (incompatible) {
      if (!noop_havoc)
        seed(memNode(o), PointsToSet::top());
    } else {
      addValSourceEdge(rhs_val, memNode(o));
    }
  };

  for (Id site : slice.inst_list) {
    const Instruction &i = *index.by_site[site].inst;
    const bool defines = i.result != INVALID && slice.values[i.result];
    switch (i.opcode) {
    case Opcode::Address:
    case Opcode::Allocate:
      if (defines) {
        seedObj(valNode(i.result), i.object);
        if (i.may_be_null)
          seedObj(valNode(i.result), NULL_OBJECT);
        for (Id op : i.operands)
          addValSourceEdge(op, valNode(i.result));
      }
      break;
    case Opcode::Unknown:
      if (defines)
        seed(valNode(i.result), PointsToSet::top());
      break;
    case Opcode::Copy:
    case Opcode::Join:
      if (defines) {
        for (Id op : i.operands)
          addValSourceEdge(op, valNode(i.result));
      }
      break;
    case Opcode::Field:
      if (defines) {
        const Id base_val = i.operands[0];
        if (base_val == INVALID) {
          seed(valNode(i.result), PointsToSet::top());
        } else if (program.values[base_val].constant) {
          const PointsToSet &bpts = program.values[base_val].initial;
          if (bpts.isTop()) {
            seed(valNode(i.result), PointsToSet::top());
          } else {
            for (Id obj : bpts.objects())
              seed(valNode(i.result),
                   program.resolveField(obj, i.field_offset, i.field_known,
                                        i.field_array_stride));
          }
        } else if (slice.values[base_val]) {
          addFieldEdge(valNode(base_val), valNode(i.result), i.field_offset,
                       i.field_known, i.field_array_stride);
        } else {
          const PointsToSet bpts = valueOf(program, coarse_upper, base_val);
          if (bpts.isTop()) {
            seed(valNode(i.result), PointsToSet::top());
          } else {
            for (Id obj : bpts.objects())
              seed(valNode(i.result),
                   program.resolveField(obj, i.field_offset, i.field_known,
                                        i.field_array_stride));
          }
        }
      }
      break;
    case Opcode::Load:
      if (defines) {
        const PointsToSet addr = valueOf(program, coarse_upper, i.operands[0]);
        if (addr.isTop()) {
          seed(valNode(i.result), PointsToSet::top());
        } else {
          for (Id obj : addr.objects()) {
            if (obj == NULL_OBJECT)
              continue;
            const Object &info = program.objects[obj];
            if (!isMemory(info) ||
                (i.read_bytes != 0 &&
                 (info.bytes == 0 || info.bytes != i.read_bytes))) {
              seed(valNode(i.result), PointsToSet::top());
            } else if (obj < slice.objects.size() && slice.objects[obj]) {
              addCopyEdge(memNode(obj), valNode(i.result));
            } else {
              seed(valNode(i.result), PointsToSet::top());
            }
          }
        }
      }
      break;
    case Opcode::Store: {
      const PointsToSet addr = valueOf(program, coarse_upper, i.operands[0]);
      if (addr.isTop()) {
        for (Id o : slice.object_list)
          addStoreToObj(o, i.operands[1], i.write_bytes);
      } else {
        for (Id o : addr.objects()) {
          addStoreToObj(o, i.operands[1], i.write_bytes);
          if (!noop_havoc && i.write_bytes != 0 &&
              isMemory(program.objects[o])) {
            for (Id fo : program.overlappingFields(o, i.write_bytes))
              if (fo != o && fo < slice.objects.size() && slice.objects[fo] &&
                  isMemory(program.objects[fo]))
                seed(memNode(fo), PointsToSet::top());
          }
        }
      }
      break;
    }
    case Opcode::MemCopy: {
      const PointsToSet dst_pts = valueOf(program, coarse_upper, i.operands[0]);
      const PointsToSet src_pts = valueOf(program, coarse_upper, i.operands[1]);
      if (dst_pts.isTop()) {
        if (!noop_havoc)
          havocAllSliceMem();
        break;
      }
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
              !program.memCopyRelativeOffsets(od, fd, i.write_bytes, rels)) {
            if (!noop_havoc)
              seed(memNode(fd), PointsToSet::top());
            continue;
          }
          for (Id os : src_pts.objects()) {
            if (os == NULL_OBJECT)
              continue;
            if (!isMemory(program.objects[os]) ||
                (os < index.has_unaligned_interior.size() &&
                 index.has_unaligned_interior[os])) {
              if (!noop_havoc)
                seed(memNode(fd), PointsToSet::top());
              continue;
            }
            for (std::int64_t rel : rels) {
              const PointsToSet fs_pts = program.resolveField(os, rel, true);
              if (fs_pts.isTop() || fs_pts.empty()) {
                if (!noop_havoc)
                  seed(memNode(fd), PointsToSet::top());
                continue;
              }
              for (Id fs : fs_pts.objects()) {
                const Object &fs_info = program.objects[fs];
                if (!isMemory(fs_info) || fs_info.bytes != fd_info.bytes ||
                    !slice.objects[fs]) {
                  if (!noop_havoc)
                    seed(memNode(fd), PointsToSet::top());
                } else {
                  addCopyEdge(memNode(fs), memNode(fd));
                }
              }
            }
          }
        }
      }
      break;
    }
    case Opcode::Havoc:
      if (defines && !noop_havoc)
        seed(valNode(i.result), PointsToSet::top());
      if (i.operands.empty()) {
        if (!noop_havoc)
          havocAllSliceMem();
      } else {
        const PointsToSet addr = valueOf(program, coarse_upper, i.operands[0]);
        if (addr.isTop()) {
          if (!noop_havoc)
            havocAllSliceMem();
        } else {
          for (Id o : addr.objects()) {
            if (!isMemory(program.objects[o]))
              continue;
            const bool o_unaligned = o < index.has_unaligned_interior.size() &&
                                     index.has_unaligned_interior[o];
            for (Id fo : program.overlappingFields(o, i.write_bytes)) {
              if (fo >= 2 && fo < slice.objects.size() && slice.objects[fo]) {
                if (i.zero_fill && !o_unaligned &&
                    program.zeroFillCoversField(o, fo, i.write_bytes))
                  seedObj(memNode(fo), NULL_OBJECT);
                else if (!noop_havoc)
                  seed(memNode(fo), PointsToSet::top());
              }
            }
          }
        }
      }
      break;
    case Opcode::Call: {
      const Targets targets = callees(
          program, i, valueOf(program, coarse_upper, i.indirect_target));
      if (targets.opaque && !noop_havoc) {
        if (defines)
          seed(valNode(i.result), PointsToSet::top());
        if (i.writes_memory)
          havocAllSliceMem();
      }
      for (Id callee_id : targets.functions) {
        const Function &callee = program.functions[callee_id];
        if (callee.external) {
          if (!noop_havoc) {
            if (defines)
              seed(valNode(i.result), PointsToSet::top());
            if (i.writes_memory && callee.writes_memory)
              havocAllSliceMem();
          }
          continue;
        }
        for (std::size_t a = 0; a < callee.parameters.size(); ++a) {
          const Id param = callee.parameters[a];
          if (param != INVALID && slice.values[param]) {
            const Id actual = a < i.operands.size() ? i.operands[a] : INVALID;
            addValSourceEdge(actual, valNode(param));
          }
        }
        if (defines) {
          for (Id ret_site : index.return_sites[callee_id]) {
            const Instruction &ret = *index.by_site[ret_site].inst;
            if (!ret.operands.empty())
              addValSourceEdge(ret.operands[0], valNode(i.result));
          }
        }
      }
      break;
    }
    default:
      break;
    }
  }

  for (Id v : slice.value_list) {
    for (const auto &phi_loc : index.phi_defs[v]) {
      const Edge &edge = program.functions[phi_loc.first]
                             .blocks[phi_loc.second.first]
                             .successors[phi_loc.second.second];
      for (auto p : edge.phi)
        if (p.first == v)
          addValSourceEdge(p.second, valNode(v));
    }
  }

  while (!work.empty()) {
    const std::size_t u = work.front();
    work.pop_front();
    in_queue[u] = false;
    const PointsToSet src_pts = pts[u];
    if (src_pts.empty())
      continue;
    for (const FlowEdge &e : adj[u]) {
      if (!e.is_field) {
        if (pts[e.dst].join(src_pts))
          enqueue(e.dst);
      } else if (src_pts.isTop()) {
        if (pts[e.dst].join(PointsToSet::top()))
          enqueue(e.dst);
      } else {
        bool changed = false;
        for (Id obj : src_pts.objects())
          changed |= pts[e.dst].join(program.resolveField(
              obj, e.field_offset, e.field_known, e.field_stride));
        if (changed)
          enqueue(e.dst);
      }
    }
  }

  Sets values(nv);
  for (Id v = 0; v < nv; ++v) {
    if (program.values[v].constant)
      values[v] = program.values[v].initial;
    else
      values[v] = std::move(pts[valNode(v)]);
  }
  return values;
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
