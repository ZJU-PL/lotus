#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

namespace {
// Unary unification graph supporting recursive shapes, self-edges, per-node
// nullability (avoiding a global NULL_OBJECT unification bridge), small-to-large
// per-root object tracking, and event-driven watcher wakeups for almost-linear
// dynamic constraint propagation.
class Unification {
  struct Node {
    Id parent;
    unsigned rank = 0;
    Id target = INVALID;
    bool top = false;
    bool nullable = false;
    std::uint32_t obj_gen = 0;
    std::vector<Id> objects;
    std::vector<std::size_t> watchers;
  };
  std::vector<Node> m_nodes;
  std::deque<std::size_t> m_ready;
  std::vector<bool> m_queued;

public:
  Id node(bool top = false) {
    Id id = nextId(m_nodes.size(), "unification node");
    m_nodes.push_back({id, 0, INVALID, top, false, 0, {}, {}});
    return id;
  }
  void addObject(Id id, Id obj) { m_nodes[find(id)].objects.push_back(obj); }
  const std::vector<Id> &objects(Id id) { return m_nodes[find(id)].objects; }
  std::uint32_t objGen(Id id) { return m_nodes[find(id)].obj_gen; }
  void initWorklist(std::size_t count) {
    m_queued.assign(count, false);
    m_ready.clear();
  }
  void enqueue(std::size_t idx) {
    if (idx < m_queued.size() && !m_queued[idx]) {
      m_queued[idx] = true;
      m_ready.push_back(idx);
    }
  }
  bool hasReady() const { return !m_ready.empty(); }
  std::size_t popReady() {
    const std::size_t idx = m_ready.front();
    m_ready.pop_front();
    m_queued[idx] = false;
    return idx;
  }
  void watch(Id id, std::size_t idx) {
    id = find(id);
    if (!m_nodes[id].top)
      m_nodes[id].watchers.push_back(idx);
  }
  void wake(Id id) {
    id = find(id);
    for (std::size_t idx : m_nodes[id].watchers)
      enqueue(idx);
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
      if (!m_nodes[id].top)
        wake(id);
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
      const bool a_was_top = m_nodes[a].top;
      const bool b_was_top = m_nodes[b].top;
      const bool a_had_objs = !m_nodes[a].objects.empty();
      const bool b_had_objs = !m_nodes[b].objects.empty();
      const bool wake_a =
          !a_was_top &&
          (b_had_objs || b_was_top ||
           (!m_nodes[a].nullable && m_nodes[b].nullable));
      const bool wake_b =
          !b_was_top &&
          (a_had_objs || a_was_top ||
           (!m_nodes[b].nullable && m_nodes[a].nullable));
      if (wake_a)
        for (std::size_t idx : m_nodes[a].watchers)
          enqueue(idx);
      if (wake_b)
        for (std::size_t idx : m_nodes[b].watchers)
          enqueue(idx);

      m_nodes[b].parent = a;
      if (m_nodes[a].rank == m_nodes[b].rank)
        ++m_nodes[a].rank;
      m_nodes[a].top |= m_nodes[b].top;
      m_nodes[a].nullable |= m_nodes[b].nullable;
      if (m_nodes[a].objects.size() < m_nodes[b].objects.size()) {
        m_nodes[a].objects.swap(m_nodes[b].objects);
        ++m_nodes[a].obj_gen;
      }
      m_nodes[a].objects.insert(m_nodes[a].objects.end(),
                                m_nodes[b].objects.begin(),
                                m_nodes[b].objects.end());
      m_nodes[b].objects.clear();
      if (m_nodes[a].top) {
        m_nodes[a].watchers.clear();
        m_nodes[b].watchers.clear();
      } else {
        if (m_nodes[a].watchers.size() < m_nodes[b].watchers.size())
          m_nodes[a].watchers.swap(m_nodes[b].watchers);
        m_nodes[a].watchers.insert(m_nodes[a].watchers.end(),
                                   m_nodes[b].watchers.begin(),
                                   m_nodes[b].watchers.end());
        m_nodes[b].watchers.clear();
      }
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
};
} // namespace

CoarseResult steensgaard(const Program &program, HavocPolicy havoc_policy) {
  const bool noop_havoc = (havoc_policy == HavocPolicy::Noop);
  Unification graph;
  std::vector<Id> object_nodes;
  object_nodes.reserve(program.objects.size());
  for (Id o = 0; o < program.objects.size(); ++o) {
    const Id n = graph.node(o == UNKNOWN);
    graph.addObject(n, o);
    object_nodes.push_back(n);
  }
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

  bool havoc_all_done = false;
  auto havoc_all = [&]() {
    if (havoc_all_done)
      return;
    havoc_all_done = true;
    for (Id o = 0; o < program.objects.size(); ++o)
      if (isMemory(program.objects[o]))
        graph.merge(graph.target(object_nodes[o]), top);
  };
  std::map<std::uint64_t, Id> top_store_hubs;
  auto getTopStoreHub = [&](std::uint64_t write_bytes) -> Id {
    auto it = top_store_hubs.find(write_bytes);
    if (it != top_store_hubs.end())
      return it->second;
    const Id hub = graph.node();
    for (Id o = 0; o < program.objects.size(); ++o) {
      if (!isMemory(program.objects[o]))
        continue;
      if (write_bytes != 0 && (program.objects[o].bytes == 0 ||
                               program.objects[o].bytes != write_bytes)) {
        if (!noop_havoc)
          graph.merge(graph.target(object_nodes[o]), top);
      } else {
        graph.merge(hub, graph.target(object_nodes[o]));
      }
    }
    top_store_hubs.emplace(write_bytes, hub);
    return hub;
  };
  std::vector<Id> addr_taken_funcs;
  for (Id f = 0; f < program.functions.size(); ++f)
    if (program.functions[f].address_taken)
      addr_taken_funcs.push_back(f);

  auto accessMatches = [&](Id address, std::uint64_t bytes,
                           bool is_load) -> bool {
    address = graph.find(address);
    if (graph.top(address))
      return false;
    const auto &objs = graph.objects(address);
    if (objs.empty())
      return true; // Wait for objects to reach this node before mismatching.
    bool found_mem = false;
    for (Id object : objs) {
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
      if (noop_havoc)
        return;
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

  // Event-driven dynamic worklist: register each deferred instruction on its
  // operand unification root(s) and re-evaluate only when a watched root gains
  // new objects, nullability, top-ness, or unaligned-interior status.
  if (!dynamic_insts.empty()) {
    std::vector<bool> unaligned_obj(program.objects.size(), false);
    for (Id o = 0; o < program.objects.size(); ++o)
      unaligned_obj[o] = program.objects[o].has_interior_pointer;

    graph.initWorklist(dynamic_insts.size());
    for (std::size_t idx = 0; idx < dynamic_insts.size(); ++idx) {
      const Instruction &i = *dynamic_insts[idx];
      graph.enqueue(idx);
      switch (i.opcode) {
      case Opcode::Field:
      case Opcode::Load:
      case Opcode::Store:
        graph.watch(value_target(i.operands[0]), idx);
        break;
      case Opcode::MemCopy:
        graph.watch(value_target(i.operands[0]), idx);
        graph.watch(value_target(i.operands[1]), idx);
        break;
      case Opcode::Havoc:
        if (!i.operands.empty())
          graph.watch(value_target(i.operands[0]), idx);
        break;
      case Opcode::Call:
        if (i.indirect_target != INVALID)
          graph.watch(value_target(i.indirect_target), idx);
        break;
      default:
        break;
      }
    }

    struct ObjCursor {
      Id root = INVALID;
      std::uint32_t gen = 0;
      std::size_t seen = 0;
    };
    std::vector<ObjCursor> cursors(dynamic_insts.size());
    std::vector<Id> objs_copy;
    std::vector<Id> src_objs_copy;
    while (graph.hasReady()) {
      const std::size_t idx = graph.popReady();
      const Instruction &i = *dynamic_insts[idx];
      switch (i.opcode) {
      case Opcode::Field: {
        const Id base_root = value_target(i.operands[0]);
        if (graph.top(base_root)) {
          graph.merge(value_target(i.result), top);
        } else {
          if (graph.nullable(base_root))
            graph.markNullable(value_target(i.result));
          const std::uint32_t cur_gen = graph.objGen(base_root);
          const auto &raw_objs = graph.objects(base_root);
          ObjCursor &cur = cursors[idx];
          const std::size_t start =
              (cur.root == base_root && cur.gen == cur_gen) ? cur.seen : 0;
          objs_copy.assign(raw_objs.begin() + static_cast<std::ptrdiff_t>(start),
                           raw_objs.end());
          cur = {base_root, cur_gen, raw_objs.size()};
          for (Id obj : objs_copy) {
            PointsToSet resolved = program.resolveField(
                obj, i.field_offset, i.field_known, i.field_array_stride);
            if (!resolved.isTop() && obj >= 2 && obj < program.objects.size()) {
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
                  graph.wake(object_nodes[target]);
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
          mergeFromValue(getTopStoreHub(i.write_bytes), i.operands[1]);
        } else if (i.write_bytes != 0) {
          if (accessMatches(address, i.write_bytes, /*is_load=*/false))
            mergeFromValue(graph.target(address), i.operands[1]);
          else if (!noop_havoc)
            graph.merge(graph.target(address), top);
          // Cross-field pointer stores (e.g., storing an 8-byte pointer at
          // offset 0 of a packed `<{ i32, i8* }>` struct) partially overwrite
          // overlapping sibling fields, which must widen to top.
          if (!noop_havoc) {
            objs_copy = graph.objects(address);
            for (Id o : objs_copy) {
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
          if (!noop_havoc)
            havoc_all();
          break;
        }
        objs_copy = graph.objects(dst_root);
        if (objs_copy.empty())
          break;
        const bool src_is_top = graph.top(src_root);
        if (!src_is_top)
          src_objs_copy = graph.objects(src_root);
        for (Id od : objs_copy) {
          if (!isMemory(program.objects[od]))
            continue;
          const auto fields = program.overlappingFields(od, i.write_bytes);
          for (Id fd : fields) {
            if (!isMemory(program.objects[fd]))
              continue;
            const Object &fd_info = program.objects[fd];
            std::vector<std::int64_t> rels;
            if (src_is_top || graph.top(value_target(i.operands[1])) ||
                unaligned_obj[od] ||
                !program.memCopyRelativeOffsets(od, fd, i.write_bytes, rels)) {
              if (!noop_havoc)
                graph.merge(graph.target(object_nodes[fd]), top);
              continue;
            }
            if (src_objs_copy.empty())
              continue;
            for (Id os : src_objs_copy) {
              if (os == NULL_OBJECT)
                continue;
              if (!isMemory(program.objects[os]) || unaligned_obj[os]) {
                if (!noop_havoc)
                  graph.merge(graph.target(object_nodes[fd]), top);
                continue;
              }
              for (std::int64_t rel : rels) {
                const PointsToSet fs_pts = program.resolveField(os, rel, true);
                if (fs_pts.isTop() || fs_pts.empty()) {
                  if (!noop_havoc)
                    graph.merge(graph.target(object_nodes[fd]), top);
                  continue;
                }
                for (Id fs : fs_pts.objects()) {
                  const Object &fs_info = program.objects[fs];
                  if (!isMemory(fs_info) || fs_info.bytes != fd_info.bytes) {
                    if (!noop_havoc)
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
        if (i.result != INVALID && !noop_havoc)
          graph.merge(value_target(i.result), top);
        if (i.operands.empty() || graph.top(value_target(i.operands[0]))) {
          if (!noop_havoc)
            havoc_all();
        } else {
          const Id addr = value_target(i.operands[0]);
          objs_copy = graph.objects(addr);
          if (objs_copy.empty())
            break;
          for (Id o : objs_copy) {
            if (!isMemory(program.objects[o]))
              continue;
            const bool o_unaligned =
                o < unaligned_obj.size() && unaligned_obj[o];
            for (Id fo : program.overlappingFields(o, i.write_bytes)) {
              if (!isMemory(program.objects[fo]))
                continue;
              if (i.zero_fill && !o_unaligned &&
                  program.zeroFillCoversField(o, fo, i.write_bytes))
                graph.markNullable(graph.target(object_nodes[fo]));
              else if (!noop_havoc)
                graph.merge(graph.target(object_nodes[fo]), top);
            }
          }
          if (!noop_havoc && !i.zero_fill && i.write_bytes == 0)
            graph.merge(graph.target(addr), top);
        }
        break;
      }
      case Opcode::Call: {
        Targets targets;
        targets.opaque = i.opaque_alternative;
        if (i.indirect_target == INVALID ||
            graph.top(value_target(i.indirect_target))) {
          for (Id f : addr_taken_funcs)
            targets.functions.insert(f);
          targets.opaque = true;
        } else {
          const Id tgt_root = value_target(i.indirect_target);
          const std::uint32_t cur_gen = graph.objGen(tgt_root);
          const auto &raw_objs = graph.objects(tgt_root);
          ObjCursor &cur = cursors[idx];
          const std::size_t start =
              (cur.root == tgt_root && cur.gen == cur_gen) ? cur.seen : 0;
          objs_copy.assign(raw_objs.begin() + static_cast<std::ptrdiff_t>(start),
                           raw_objs.end());
          cur = {tgt_root, cur_gen, raw_objs.size()};
          for (Id obj : objs_copy) {
            if (program.objects[obj].kind == ObjectKind::Function)
              targets.functions.insert(program.objects[obj].function);
            else if (obj != NULL_OBJECT)
              targets.opaque = true;
          }
        }
        if (targets.opaque && !noop_havoc) {
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

  result.object_upper.resize(program.objects.size());
  for (Id o = 0; o < program.objects.size(); ++o) {
    const Id tgt = graph.existingTarget(object_nodes[o]);
    if (tgt == INVALID) {
      result.object_upper[o] = program.objects[o].initial;
    } else if (graph.top(tgt)) {
      result.object_upper[o] = PointsToSet::top();
    } else {
      const Id tcomp = component[rootNumber.at(graph.find(tgt))];
      result.object_upper[o] = objects_in_component[tcomp];
      if (graph.nullable(tgt))
        result.object_upper[o].insert(NULL_OBJECT);
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
