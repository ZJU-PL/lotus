#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

namespace {
// Unary unification graph supporting recursive shapes, self-edges, and
// per-node nullability (avoiding a global NULL_OBJECT unification bridge).
class Unification {
  struct Node {
    Id parent;
    unsigned rank = 0;
    Id target = INVALID;
    bool top = false;
    bool nullable = false;
  };
  std::vector<Node> m_nodes;
  std::size_t m_version = 0;

public:
  Id node(bool top = false) {
    Id id = nextId(m_nodes.size(), "unification node");
    m_nodes.push_back({id, 0, INVALID, top, false});
    ++m_version;
    return id;
  }
  std::size_t size() const { return m_nodes.size(); }
  Id existingTarget(Id id) {
    id = find(id);
    return m_nodes[id].target == INVALID ? INVALID : find(m_nodes[id].target);
  }
  Id find(Id id) {
    Id root = id;
    while (m_nodes[root].parent != root)
      root = m_nodes[root].parent;
    while (id != root) {
      Id next = m_nodes[id].parent;
      m_nodes[id].parent = root;
      id = next;
    }
    return root;
  }
  bool top(Id id) { return m_nodes[find(id)].top; }
  bool nullable(Id id) { return m_nodes[find(id)].nullable; }
  void markNullable(Id id) {
    id = find(id);
    if (!m_nodes[id].nullable) {
      m_nodes[id].nullable = true;
      ++m_version;
    }
  }
  Id target(Id id) {
    id = find(id);
    if (m_nodes[id].top)
      return id;
    if (m_nodes[id].target == INVALID) {
      Id fresh = node();
      m_nodes[id].target = fresh;
    }
    return find(m_nodes[id].target);
  }
  void merge(Id lhs, Id rhs) {
    std::deque<std::pair<Id, Id>> work{{lhs, rhs}};
    while (!work.empty()) {
      auto pair = work.front();
      work.pop_front();
      Id a = find(pair.first), b = find(pair.second);
      if (a == b)
        continue;
      if (m_nodes[a].rank < m_nodes[b].rank)
        std::swap(a, b);
      Id ta = m_nodes[a].target, tb = m_nodes[b].target;
      m_nodes[b].parent = a;
      if (m_nodes[a].rank == m_nodes[b].rank)
        ++m_nodes[a].rank;
      m_nodes[a].top |= m_nodes[b].top;
      m_nodes[a].nullable |= m_nodes[b].nullable;
      ++m_version;
      if (m_nodes[a].top) {
        m_nodes[a].target = a;
        if (ta != INVALID)
          work.emplace_back(a, ta);
        if (tb != INVALID)
          work.emplace_back(a, tb);
      } else {
        m_nodes[a].target = ta == INVALID ? tb : ta;
        if (ta != INVALID && tb != INVALID)
          work.emplace_back(ta, tb);
      }
    }
  }
  std::size_t version() const { return m_version; }
};
} // namespace

CoarseResult steensgaard(const Program &program) {
  Unification graph;
  std::vector<Id> object_nodes;
  object_nodes.reserve(program.objects.size());
  for (Id o = 0; o < program.objects.size(); ++o)
    object_nodes.push_back(graph.node(o == UNKNOWN));
  // Represent each Value directly by its points-to target unification node so
  // no unmerged dummy value-wrapper nodes pollute SteensgaardHierarchy.
  std::vector<Id> value_targets;
  value_targets.reserve(program.values.size());
  for (std::size_t v = 0; v < program.values.size(); ++v)
    value_targets.push_back(graph.node());

  const Id top = object_nodes[UNKNOWN];
  auto isNullOnlyConstant = [&](Id value) -> bool {
    return value != INVALID && program.values[value].constant &&
           !program.values[value].initial.isTop() &&
           program.values[value].initial.size() == 1 &&
           program.values[value].initial.contains(NULL_OBJECT);
  };
  auto value_target = [&](Id value) -> Id {
    return value == INVALID ? top : graph.find(value_targets[value]);
  };
  auto mergeFromValue = [&](Id dst_node, Id src_val) {
    if (isNullOnlyConstant(src_val))
      graph.markNullable(dst_node);
    else
      graph.merge(dst_node, value_target(src_val));
  };
  auto seed_object = [&](Id obj, const PointsToSet &set) {
    for (Id target : set.objects()) {
      if (target == NULL_OBJECT)
        graph.markNullable(graph.target(object_nodes[obj]));
      else
        graph.merge(graph.target(object_nodes[obj]), object_nodes[target]);
    }
  };
  auto seed_value = [&](Id v, const PointsToSet &set) {
    for (Id target : set.objects()) {
      if (target == NULL_OBJECT)
        graph.markNullable(value_targets[v]);
      else
        graph.merge(value_targets[v], object_nodes[target]);
    }
  };

  for (Id o = 0; o < program.objects.size(); ++o)
    seed_object(o, program.objects[o].initial);
  for (Id v = 0; v < program.values.size(); ++v)
    if (program.values[v].constant)
      seed_value(v, program.values[v].initial);
  for (Id parameter : program.functions[program.entry].parameters)
    if (parameter != INVALID)
      graph.merge(value_targets[parameter], top);

  auto havoc_all = [&]() {
    for (Id o = 0; o < program.objects.size(); ++o)
      if (isMemory(program.objects[o]))
        graph.merge(graph.target(object_nodes[o]), top);
  };

  std::unordered_map<Id, std::vector<Id>> objects_by_root;
  auto rebuild_root_objects = [&]() {
    objects_by_root.clear();
    for (Id o = 0; o < program.objects.size(); ++o)
      objects_by_root[graph.find(object_nodes[o])].push_back(o);
  };
  auto accessMatches = [&](Id address, std::uint64_t bytes,
                           bool is_load) -> bool {
    address = graph.find(address);
    if (graph.top(address))
      return false;
    auto it = objects_by_root.find(address);
    if (it == objects_by_root.end())
      return true; // Wait for objects to reach this node before mismatching.
    bool found_mem = false;
    for (Id object : it->second) {
      if (object == NULL_OBJECT)
        continue;
      if (!isMemory(program.objects[object])) {
        if (is_load)
          return false; // Loading from a non-memory object (e.g. Function) yields top.
        continue;
      }
      found_mem = true;
      if (bytes != 0 && (program.objects[object].bytes == 0 ||
                         program.objects[object].bytes != bytes))
        return false;
    }
    return bytes == 0 ? true : found_mem;
  };

  auto applyDirectCall = [&](const Instruction &i, Id f) {
    const Function &callee = program.functions[f];
    if (callee.external) {
      if (i.result != INVALID)
        graph.merge(value_target(i.result), top);
      if (i.writes_memory && callee.writes_memory)
        havoc_all();
      return;
    }
    for (std::size_t a = 0; a < callee.parameters.size(); ++a) {
      if (callee.parameters[a] != INVALID) {
        const Id actual = a < i.operands.size() ? i.operands[a] : INVALID;
        mergeFromValue(value_target(callee.parameters[a]), actual);
      }
    }
    if (i.result != INVALID)
      for (const Block &block : callee.blocks)
        for (const Instruction &ret : block.instructions)
          if (ret.opcode == Opcode::Return && !ret.operands.empty())
            mergeFromValue(value_target(i.result), ret.operands[0]);
  };

  // Pass 1 (almost-linear single scan): process all static unification
  // constraints directly and collect only dynamic instructions whose effect
  // depends on object-set membership or root top-ness (Field, MemCopy, Havoc,
  // indirect Call, byte-sized Load/Store, and Stores whose address may become
  // top).
  std::vector<const Instruction *> dynamic_insts;
  forInstructions(program, [&](Id, const Instruction &i) {
    switch (i.opcode) {
    case Opcode::Address:
    case Opcode::Allocate:
      if (i.object == NULL_OBJECT)
        graph.markNullable(value_target(i.result));
      else
        graph.merge(value_target(i.result), object_nodes[i.object]);
      if (i.may_be_null)
        graph.markNullable(value_target(i.result));
      for (Id operand : i.operands)
        mergeFromValue(value_target(i.result), operand);
      break;
    case Opcode::Unknown:
      graph.merge(value_target(i.result), top);
      break;
    case Opcode::Copy:
    case Opcode::Join:
      for (Id operand : i.operands)
        mergeFromValue(value_target(i.result), operand);
      break;
    case Opcode::Field:
      if (i.field_known && i.field_offset == 0 && i.field_array_stride == 0) {
        mergeFromValue(value_target(i.result), i.operands[0]);
      } else {
        dynamic_insts.push_back(&i);
      }
      break;
    case Opcode::Load: {
      const Id addr = value_target(i.operands[0]);
      if (i.read_bytes == 0 && !graph.top(addr))
        graph.merge(value_target(i.result), graph.target(addr));
      // Re-check for byte-size mismatch or non-memory (Function) targets.
      dynamic_insts.push_back(&i);
      break;
    }
    case Opcode::Store: {
      const Id address = value_target(i.operands[0]);
      if (i.write_bytes == 0 && !graph.top(address)) {
        mergeFromValue(graph.target(address), i.operands[1]);
      }
      // Re-check if address is/becomes top (writes all memory) or byte-sized.
      dynamic_insts.push_back(&i);
      break;
    }
    case Opcode::MemCopy:
    case Opcode::Havoc:
      dynamic_insts.push_back(&i);
      break;
    case Opcode::Call:
      if (i.opaque_alternative) {
        if (i.result != INVALID)
          graph.merge(value_target(i.result), top);
        if (i.writes_memory)
          havoc_all();
      }
      if (i.callee != INVALID) {
        applyDirectCall(i, i.callee);
      } else {
        dynamic_insts.push_back(&i);
      }
      break;
    default:
      break;
    }
  });
  for (const Function &function : program.functions)
    for (const Block &block : function.blocks)
      for (const Edge &edge : block.successors)
        for (auto phi : edge.phi)
          mergeFromValue(value_target(phi.first), phi.second);

  // Dynamic fixpoint: iterate ONLY over deferred dynamic instructions.
  if (!dynamic_insts.empty()) {
    std::vector<bool> unaligned_obj(program.objects.size(), false);
    for (Id o = 0; o < program.objects.size(); ++o)
      unaligned_obj[o] = program.objects[o].has_interior_pointer;
    std::size_t old = 0;
    bool unaligned_changed = false;
    do {
      old = graph.version();
      unaligned_changed = false;
      rebuild_root_objects();
      for (const Instruction *ip : dynamic_insts) {
        const Instruction &i = *ip;
        switch (i.opcode) {
        case Opcode::Field: {
          const Id base_root = value_target(i.operands[0]);
          if (graph.top(base_root)) {
            graph.merge(value_target(i.result), top);
          } else {
            if (graph.nullable(base_root))
              graph.markNullable(value_target(i.result));
            auto it = objects_by_root.find(base_root);
            if (it != objects_by_root.end()) {
              for (Id obj : it->second) {
                PointsToSet resolved = program.resolveField(
                    obj, i.field_offset, i.field_known, i.field_array_stride);
                if (!resolved.isTop() && obj >= 2 &&
                    obj < program.objects.size()) {
                  const Id b = program.baseOf(obj);
                  const bool non_fs = !program.objects[b].field_sensitive;
                  for (Id target : resolved.objects()) {
                    if (target == NULL_OBJECT || target >= unaligned_obj.size())
                      continue;
                    bool mark = false;
                    if (non_fs) {
                      mark = true;
                    } else if (!i.field_known) {
                      const std::uint64_t align = program.objects[target].bytes;
                      const bool off_ok =
                          i.field_offset == 0 ||
                          (align > 0 &&
                           align <=
                               static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max()) &&
                           (i.field_offset %
                            static_cast<std::int64_t>(align)) == 0);
                      if (i.field_array_stride == 0 || align == 0 ||
                          (i.field_array_stride % align) != 0 || !off_ok)
                        mark = true;
                    } else {
                      const std::uint64_t co = program.objects[obj].offset;
                      if (co <= static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max())) {
                        const std::int64_t cs = static_cast<std::int64_t>(co);
                        const bool ok =
                            !((i.field_offset > 0 &&
                               cs > std::numeric_limits<std::int64_t>::max() -
                                        i.field_offset) ||
                              (i.field_offset < 0 &&
                               cs < std::numeric_limits<std::int64_t>::min() -
                                        i.field_offset));
                        if (ok && cs + i.field_offset >= 0 &&
                            program.objects[target].offset !=
                                program.foldOffset(
                                    b, static_cast<std::uint64_t>(
                                           cs + i.field_offset)))
                          mark = true;
                      }
                    }
                    if (mark && !unaligned_obj[target]) {
                      unaligned_obj[target] = true;
                      unaligned_changed = true;
                    }
                  }
                }
                for (Id target : resolved.objects()) {
                  if (target == NULL_OBJECT)
                    graph.markNullable(value_target(i.result));
                  else
                    graph.merge(value_target(i.result), object_nodes[target]);
                }
              }
            }
          }
          break;
        }
        case Opcode::Load: {
          const Id addr = value_target(i.operands[0]);
          graph.merge(value_target(i.result),
                      accessMatches(addr, i.read_bytes, /*is_load=*/true)
                          ? graph.target(addr)
                          : top);
          break;
        }
        case Opcode::Store: {
          const Id address = value_target(i.operands[0]);
          if (graph.top(address)) {
            for (Id o = 0; o < program.objects.size(); ++o)
              if (isMemory(program.objects[o])) {
                if (i.write_bytes)
                  graph.merge(graph.target(object_nodes[o]), top);
                else
                  mergeFromValue(graph.target(object_nodes[o]), i.operands[1]);
              }
          } else if (i.write_bytes != 0) {
            if (accessMatches(address, i.write_bytes, /*is_load=*/false))
              mergeFromValue(graph.target(address), i.operands[1]);
            else
              graph.merge(graph.target(address), top);
            // Cross-field pointer stores (e.g., storing an 8-byte pointer at
            // offset 0 of a packed `<{ i32, i8* }>` struct) partially overwrite
            // overlapping sibling fields, which must widen to top.
            auto it = objects_by_root.find(address);
            if (it != objects_by_root.end()) {
              for (Id o : it->second) {
                if (!isMemory(program.objects[o]))
                  continue;
                for (Id fo : program.overlappingFields(o, i.write_bytes))
                  if (fo != o && isMemory(program.objects[fo]))
                    graph.merge(graph.target(object_nodes[fo]), top);
              }
            }
          }
          break;
        }
        case Opcode::MemCopy: {
          const Id dst_root = value_target(i.operands[0]);
          const Id src_root = value_target(i.operands[1]);
          if (graph.top(dst_root)) {
            havoc_all();
            break;
          }
          auto dst_it = objects_by_root.find(dst_root);
          if (dst_it == objects_by_root.end())
            break;
          for (Id od : dst_it->second) {
            if (!isMemory(program.objects[od]))
              continue;
            const auto fields = program.overlappingFields(od, i.write_bytes);
            for (Id fd : fields) {
              if (!isMemory(program.objects[fd]))
                continue;
              const Object &fd_info = program.objects[fd];
              std::vector<std::int64_t> rels;
              if (graph.top(src_root) || unaligned_obj[od] ||
                  !program.memCopyRelativeOffsets(od, fd, i.write_bytes,
                                                  rels)) {
                graph.merge(graph.target(object_nodes[fd]), top);
                continue;
              }
              auto src_it = objects_by_root.find(src_root);
              if (src_it == objects_by_root.end())
                continue;
              for (Id os : src_it->second) {
                if (!isMemory(program.objects[os]) || unaligned_obj[os]) {
                  graph.merge(graph.target(object_nodes[fd]), top);
                  continue;
                }
                for (std::int64_t rel : rels) {
                  const PointsToSet fs_pts =
                      program.resolveField(os, rel, true);
                  if (fs_pts.isTop() || fs_pts.empty()) {
                    graph.merge(graph.target(object_nodes[fd]), top);
                    continue;
                  }
                  for (Id fs : fs_pts.objects()) {
                    const Object &fs_info = program.objects[fs];
                    if (!isMemory(fs_info) || fs_info.bytes != fd_info.bytes) {
                      graph.merge(graph.target(object_nodes[fd]), top);
                    } else {
                      graph.merge(graph.target(object_nodes[fd]),
                                  graph.target(object_nodes[fs]));
                    }
                  }
                }
              }
            }
          }
          break;
        }
        case Opcode::Havoc: {
          if (i.result != INVALID)
            graph.merge(value_target(i.result), top);
          if (i.operands.empty() || graph.top(value_target(i.operands[0]))) {
            havoc_all();
          } else {
            const Id addr = value_target(i.operands[0]);
            auto it = objects_by_root.find(addr);
            if (it != objects_by_root.end()) {
              for (Id o : it->second) {
                if (!isMemory(program.objects[o]))
                  continue;
                for (Id fo : program.overlappingFields(o, i.write_bytes))
                  if (isMemory(program.objects[fo]))
                    graph.merge(graph.target(object_nodes[fo]), top);
              }
            }
            if (i.write_bytes == 0 || it == objects_by_root.end())
              graph.merge(graph.target(addr), top);
          }
          break;
        }
        case Opcode::Call: {
          Targets targets;
          targets.opaque = i.opaque_alternative;
          if (i.indirect_target == INVALID ||
              graph.top(value_target(i.indirect_target))) {
            for (Id f = 0; f < program.functions.size(); ++f)
              targets.functions.insert(f);
            targets.opaque = true;
          } else {
            auto it = objects_by_root.find(value_target(i.indirect_target));
            if (it != objects_by_root.end()) {
              for (Id obj : it->second) {
                if (program.objects[obj].kind == ObjectKind::Function)
                  targets.functions.insert(program.objects[obj].function);
                else if (obj != NULL_OBJECT)
                  targets.opaque = true;
              }
            }
          }
          if (targets.opaque) {
            if (i.result != INVALID)
              graph.merge(value_target(i.result), top);
            if (i.writes_memory)
              havoc_all();
          }
          for (Id f : targets.functions)
            applyDirectCall(i, f);
          break;
        }
        default:
          break;
        }
      }
    } while (old != graph.version() || unaligned_changed);
  }

  CoarseResult result;
  std::map<Id, Id> rootNumber;
  for (Id node = 0; node < graph.size(); ++node) {
    const Id root = graph.find(node);
    if (!rootNumber.count(root))
      rootNumber.emplace(root, nextId(rootNumber.size(), "hierarchy node"));
  }
  std::vector<std::vector<Id>> rawSuccessors(rootNumber.size());
  std::vector<std::vector<Id>> rawPredecessors(rootNumber.size());
  for (const auto &[root, from] : rootNumber) {
    const Id target = graph.existingTarget(root);
    if (target == INVALID)
      continue;
    const Id to = rootNumber.at(graph.find(target));
    if (std::find(rawSuccessors[from].begin(), rawSuccessors[from].end(), to) ==
        rawSuccessors[from].end()) {
      rawSuccessors[from].push_back(to);
      rawPredecessors[to].push_back(from);
    }
  }

  std::vector<Id> component;
  Id componentCount = 0;
  computeKosarajuSCCs(rawSuccessors.size(), rawSuccessors, rawPredecessors,
                      component, componentCount);

  auto &hierarchy = result.hierarchy;
  hierarchy.successors.resize(componentCount);
  hierarchy.predecessors.resize(componentCount);
  hierarchy.values.resize(componentCount);
  hierarchy.depth.assign(componentCount, 0);
  hierarchy.cyclic.assign(componentCount, false);
  std::vector<std::size_t> componentSize(componentCount, 0);
  for (Id node = 0; node < rawSuccessors.size(); ++node) {
    const Id from = component[node];
    ++componentSize[from];
    for (Id rawTarget : rawSuccessors[node]) {
      const Id to = component[rawTarget];
      if (from == to) {
        hierarchy.cyclic[from] = true;
      } else if (std::find(hierarchy.successors[from].begin(),
                           hierarchy.successors[from].end(),
                           to) == hierarchy.successors[from].end()) {
        hierarchy.successors[from].push_back(to);
        hierarchy.predecessors[to].push_back(from);
      }
    }
  }
  for (Id node = 0; node < componentCount; ++node)
    hierarchy.cyclic[node] = hierarchy.cyclic[node] || componentSize[node] > 1;

  std::vector<std::size_t> indegree(componentCount, 0);
  std::deque<Id> hierarchyWork;
  for (Id node = 0; node < componentCount; ++node) {
    indegree[node] = hierarchy.predecessors[node].size();
    if (indegree[node] == 0)
      hierarchyWork.push_back(node);
  }
  while (!hierarchyWork.empty()) {
    const Id node = hierarchyWork.front();
    hierarchyWork.pop_front();
    for (Id successor : hierarchy.successors[node]) {
      hierarchy.depth[successor] =
          std::max(hierarchy.depth[successor], hierarchy.depth[node] + 1);
      if (--indegree[successor] == 0)
        hierarchyWork.push_back(successor);
    }
  }

  hierarchy.value_components.resize(program.values.size());
  for (Id value = 0; value < program.values.size(); ++value) {
    const Id raw = rootNumber.at(graph.find(value_targets[value]));
    hierarchy.value_components[value] = component[raw];
    hierarchy.values[component[raw]].push_back(value);
  }
  hierarchy.object_components.resize(program.objects.size());
  for (Id object = 0; object < program.objects.size(); ++object) {
    const Id raw = rootNumber.at(graph.find(object_nodes[object]));
    hierarchy.object_components[object] = component[raw];
  }
  hierarchy.top_component =
      component[rootNumber.at(graph.find(object_nodes[UNKNOWN]))];

  std::vector<PointsToSet> objects_in_component(componentCount);
  for (Id o = 0; o < program.objects.size(); ++o)
    objects_in_component[hierarchy.object_components[o]].insert(o);

  result.upper.resize(program.values.size());
  for (Id v = 0; v < program.values.size(); ++v) {
    const Id target = graph.find(value_targets[v]);
    if (graph.top(target)) {
      result.upper[v] = PointsToSet::top();
    } else {
      result.upper[v] = objects_in_component[hierarchy.value_components[v]];
      if (graph.nullable(target))
        result.upper[v].insert(NULL_OBJECT);
    }
  }

  // Keep every Steensgaard equivalence class as its own partition instead of
  // collapsing the entire program whenever a single top pointer (e.g. main's
  // argv) exists. Soundness for stores through top and mayAlias with top is
  // preserved directly by relevant() and mayAlias()/aliases().
  for (const auto &values : hierarchy.values)
    if (!values.empty())
      result.partitions.push_back(values);
  return result;
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
