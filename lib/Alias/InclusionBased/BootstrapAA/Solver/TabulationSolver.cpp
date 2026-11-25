#include "Alias/InclusionBased/BootstrapAA/Solver/ClusterSolver.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace lotus {
namespace bootstrap {
namespace detail {

namespace {

struct State {
  bool reachable = false;
  Sets values, memory;
  State(std::size_t nv, std::size_t no) : values(nv), memory(no) {}
  bool join(const State &other) {
    if (!other.reachable)
      return false;
    bool changed = !reachable;
    reachable = true;
    for (std::size_t i = 0; i < values.size(); ++i)
      changed |= values[i].join(other.values[i]);
    for (std::size_t i = 0; i < memory.size(); ++i)
      changed |= memory[i].join(other.memory[i]);
    return changed;
  }
};

struct Summary {
  bool reachable = false;
  PointsToSet returned;
  Sets memory;
  explicit Summary(std::size_t size) : memory(size) {}
  bool join(const Summary &other) {
    if (!other.reachable)
      return false;
    bool changed = !reachable;
    reachable = true;
    changed |= returned.join(other.returned);
    for (std::size_t i = 0; i < memory.size(); ++i)
      changed |= memory[i].join(other.memory[i]);
    return changed;
  }
};

struct Key {
  Id function;
  Sets parameters, memory;
  bool operator<(const Key &other) const {
    return std::tie(function, parameters, memory) <
           std::tie(other.function, other.parameters, other.memory);
  }
};

struct Record {
  Key key;
  State entry;
  Summary summary;
  std::set<std::size_t> callers;
  std::map<Id, std::set<std::size_t>> calls;
  std::map<Id, std::pair<State, State>> snapshots;
  Record(Key k, State e)
      : key(std::move(k)), entry(std::move(e)), summary(entry.memory.size()) {}
};

// Reference forward input-state tabulation solver (retained as a differential
// verification oracle and optional backend via SolverMode::InputStateTabulation).
class TabulationSolver final : public ClusterSolver {
  const Program &m_program;
  const ProgramIndex &m_index;
  const Sets &m_upper;
  const CallGraphInfo &m_call_graph;
  std::vector<Id> m_cluster_values;
  const Slice m_slice;
  const Options &m_options;
  Statistics m_stats;
  std::vector<std::unique_ptr<Record>> m_records;
  std::map<Key, std::size_t> m_keys;
  std::map<std::size_t, std::deque<std::size_t>, std::greater<std::size_t>>
      m_work;
  std::set<std::size_t> m_queued;
  std::size_t m_steps = 0;
  bool m_failed = false;

  State bottom() const {
    return State(m_program.values.size(), m_program.objects.size());
  }
  void tick() {
    ++m_stats.steps;
    if (++m_steps > m_options.max_steps)
      throw LimitReached{};
  }
  void schedule(std::size_t context) {
    if (m_queued.insert(context).second)
      m_work[m_call_graph.function_rank[m_records[context]->key.function]]
          .push_back(context);
  }
  std::size_t takeScheduled() {
    auto first = m_work.begin();
    const std::size_t context = first->second.front();
    first->second.pop_front();
    if (first->second.empty())
      m_work.erase(first);
    m_queued.erase(context);
    return context;
  }
  std::size_t context(Key key) {
    auto it = m_keys.find(key);
    if (it != m_keys.end()) {
      ++m_stats.context_cache_hits;
      return it->second;
    }
    if (m_records.size() >= m_options.max_contexts)
      throw LimitReached{};
    State entry = bottom();
    entry.reachable = true;
    entry.memory = key.memory;
    const auto &parameters = m_program.functions[key.function].parameters;
    for (std::size_t a = 0; a < parameters.size(); ++a)
      if (parameters[a] != INVALID && m_slice.values[parameters[a]])
        entry.values[parameters[a]] = key.parameters[a];
    const std::size_t id = m_records.size();
    m_keys.emplace(key, id);
    m_records.emplace_back(
        std::make_unique<Record>(std::move(key), std::move(entry)));
    ++m_stats.contexts;
    ++m_stats.context_cache_misses;
    m_stats.maximum_contexts_per_cluster =
        std::max(m_stats.maximum_contexts_per_cluster, m_records.size());
    schedule(id);
    return id;
  }
  PointsToSet eval(const State &state, Id value) const {
    if (value != INVALID && !m_program.values[value].constant &&
        !m_slice.values[value])
      return valueOf(m_program, m_upper, value);
    return valueOf(m_program, state.values, value);
  }
  void writeObject(State &state, Id object, const PointsToSet &rhs,
                   std::uint64_t bytes, bool strong_allowed) const {
    if (object < 2 || object >= m_slice.objects.size() ||
        !m_slice.objects[object])
      return;
    const Object &info = m_program.objects[object];
    if (!isMemory(info))
      return;
    const bool strong =
        strong_allowed && object < m_index.exact_address_singleton.size() &&
        m_index.exact_address_singleton[object] &&
        (bytes == 0 || (info.bytes != 0 && bytes == info.bytes));
    if (bytes != 0 && (info.bytes == 0 || info.bytes != bytes))
      state.memory[object] = PointsToSet::top();
    else if (strong)
      state.memory[object] = rhs;
    else
      state.memory[object].join(rhs);
  }
  void write(State &state, const PointsToSet &address, const PointsToSet &rhs,
             std::uint64_t bytes, bool allow_strong) const {
    if (address.empty())
      return;
    if (address.isTop()) {
      for (Id object : m_slice.object_list)
        writeObject(state, object, rhs, bytes, false);
      return;
    }
    const bool single = allow_strong && address.size() == 1;
    for (Id object : address.objects()) {
      writeObject(state, object, rhs, bytes, single);
      if (bytes != 0 && isMemory(m_program.objects[object])) {
        for (Id fo : m_program.overlappingFields(object, bytes))
          if (fo != object)
            writeObject(state, fo, PointsToSet::top(), 0, false);
      }
    }
  }
  State external(const Instruction &instruction, State state,
                 bool writes) const {
    if (writes && instruction.writes_memory)
      write(state, PointsToSet::top(), PointsToSet::top(), 0, false);
    if (instruction.result != INVALID && m_slice.values[instruction.result])
      state.values[instruction.result] = PointsToSet::top();
    return state;
  }
  State call(std::size_t caller, const Instruction &instruction,
             const State &state) {
    State result = bottom();
    Targets targets = callees(m_program, instruction,
                              eval(state, instruction.indirect_target));
    std::set<std::size_t> edges;
    if (targets.opaque)
      result.join(external(instruction, state, true));
    for (Id f : targets.functions) {
      const Function &function = m_program.functions[f];
      if (function.external) {
        result.join(external(instruction, state, function.writes_memory));
        continue;
      }
      Key key{f, Sets(function.parameters.size()), state.memory};
      for (std::size_t a = 0; a < function.parameters.size(); ++a)
        if (function.parameters[a] != INVALID &&
            m_slice.values[function.parameters[a]])
          key.parameters[a] = eval(state, a < instruction.operands.size()
                                              ? instruction.operands[a]
                                              : INVALID);
      const std::size_t callee = context(std::move(key));
      edges.insert(callee);
      m_records[callee]->callers.insert(caller);
      const Summary &summary = m_records[callee]->summary;
      if (!summary.reachable)
        continue;
      State returned = state;
      returned.memory = summary.memory;
      if (instruction.result != INVALID && m_slice.values[instruction.result])
        returned.values[instruction.result] = summary.returned;
      result.join(returned);
    }
    m_records[caller]->calls[instruction.id] = std::move(edges);
    return result;
  }
  State transfer(std::size_t id, const Instruction &i, State state) {
    tick();
    const bool defines = i.result != INVALID && m_slice.values[i.result];
    PointsToSet rhs;
    switch (i.opcode) {
    case Opcode::Address:
      rhs.insert(i.object);
      break;
    case Opcode::Allocate: {
      rhs.insert(i.object);
      if (i.may_be_null)
        rhs.insert(NULL_OBJECT);
      for (Id operand : i.operands)
        rhs.join(eval(state, operand));
      const Id base = m_program.baseOf(i.object);
      auto initObj = [&](Id obj) {
        if (obj < m_slice.objects.size() && m_slice.objects[obj]) {
          if (m_program.objects[obj].singleton)
            state.memory[obj] = m_program.objects[obj].initial;
          else
            state.memory[obj].join(m_program.objects[obj].initial);
        }
      };
      if (base < m_program.fields_of_base.size() &&
          !m_program.fields_of_base[base].empty()) {
        for (auto [off, fo] : m_program.fields_of_base[base])
          initObj(fo);
      } else {
        initObj(i.object);
      }
      break;
    }
    case Opcode::Unknown:
      rhs = PointsToSet::top();
      break;
    case Opcode::Copy:
    case Opcode::Join:
      if (defines)
        for (Id operand : i.operands)
          rhs.join(eval(state, operand));
      break;
    case Opcode::Field:
      if (defines) {
        const PointsToSet base_pts = eval(state, i.operands[0]);
        if (base_pts.isTop()) {
          rhs = PointsToSet::top();
        } else {
          for (Id obj : base_pts.objects())
            rhs.join(m_program.resolveField(obj, i.field_offset, i.field_known,
                                            i.field_array_stride));
        }
      }
      break;
    case Opcode::Load:
      if (defines) {
        const PointsToSet address = eval(state, i.operands[0]);
        if (address.isTop())
          rhs = PointsToSet::top();
        else
          for (Id object : address.objects()) {
            if (object == NULL_OBJECT)
              continue;
            const Object &info = m_program.objects[object];
            if (!isMemory(info) ||
                (i.read_bytes != 0 &&
                 (info.bytes == 0 || info.bytes != i.read_bytes)))
              rhs.join(PointsToSet::top());
            else
              rhs.join(state.memory[object]);
          }
      }
      break;
    case Opcode::Store: {
      const PointsToSet upper_addr =
          valueOf(m_program, m_upper, i.operands[0]);
      bool store_touches = touches(m_program, m_slice, upper_addr);
      if (!store_touches && i.write_bytes != 0 && !upper_addr.isTop()) {
        for (Id o : upper_addr.objects()) {
          if (!isMemory(m_program.objects[o]))
            continue;
          for (Id fo : m_program.overlappingFields(o, i.write_bytes))
            if (fo >= 2 && fo < m_slice.objects.size() && m_slice.objects[fo]) {
              store_touches = true;
              break;
            }
          if (store_touches)
            break;
        }
      }
      if (store_touches)
        write(state, eval(state, i.operands[0]), eval(state, i.operands[1]),
              i.write_bytes, true);
      return state;
    }
    case Opcode::MemCopy: {
      const PointsToSet dst_pts = eval(state, i.operands[0]);
      const PointsToSet src_pts = eval(state, i.operands[1]);
      if (dst_pts.isTop()) {
        write(state, PointsToSet::top(), PointsToSet::top(), 0, false);
        return state;
      }
      // Capture pre-copy memory snapshot for memmove overlap soundness.
      const Sets prev_memory = state.memory;
      const bool single_dst = dst_pts.size() == 1;
      for (Id od : dst_pts.objects()) {
        if (!isMemory(m_program.objects[od]))
          continue;
        for (Id fd : m_program.overlappingFields(od, i.write_bytes)) {
          if (!m_slice.objects[fd])
            continue;
          const Object &fd_info = m_program.objects[fd];
          std::vector<std::int64_t> rels;
          const bool dst_unaligned =
              od < m_index.has_unaligned_interior.size() &&
              m_index.has_unaligned_interior[od];
          if (src_pts.isTop() || dst_unaligned ||
              !m_program.memCopyRelativeOffsets(od, fd, i.write_bytes, rels)) {
            state.memory[fd] = PointsToSet::top();
            continue;
          }
          PointsToSet copied;
          for (Id os : src_pts.objects()) {
            if (!isMemory(m_program.objects[os]) ||
                (os < m_index.has_unaligned_interior.size() &&
                 m_index.has_unaligned_interior[os])) {
              copied = PointsToSet::top();
              break;
            }
            for (std::int64_t rel : rels) {
              const PointsToSet fs_pts = m_program.resolveField(os, rel, true);
              if (fs_pts.isTop() || fs_pts.empty()) {
                copied = PointsToSet::top();
                break;
              }
              for (Id fs : fs_pts.objects()) {
                const Object &fs_info = m_program.objects[fs];
                if (!isMemory(fs_info) || fs_info.bytes != fd_info.bytes ||
                    !m_slice.objects[fs])
                  copied = PointsToSet::top();
                else
                  copied.join(prev_memory[fs]);
              }
            }
            if (copied.isTop())
              break;
          }
          if (single_dst && fd < m_index.exact_address_singleton.size() &&
              m_index.exact_address_singleton[fd])
            state.memory[fd] = std::move(copied);
          else
            state.memory[fd].join(copied);
        }
      }
      return state;
    }
    case Opcode::Havoc:
      if (defines)
        state.values[i.result] = PointsToSet::top();
      if (i.operands.empty()) {
        write(state, PointsToSet::top(), PointsToSet::top(), 0, false);
      } else {
        const PointsToSet addr = eval(state, i.operands[0]);
        if (addr.isTop()) {
          write(state, PointsToSet::top(), PointsToSet::top(), 0, false);
        } else {
          for (Id o : addr.objects()) {
            if (!isMemory(m_program.objects[o]))
              continue;
            for (Id fo : m_program.overlappingFields(o, i.write_bytes))
              writeObject(state, fo, PointsToSet::top(), 0, false);
          }
        }
      }
      return state;
    case Opcode::Call:
      return call(id, i, state);
    default:
      return state;
    }
    if (defines)
      state.values[i.result] = std::move(rhs);
    return state;
  }
  Summary evaluate(std::size_t id) {
    tick();
    Record &record = *m_records[id];
    record.snapshots.clear();
    record.calls.clear();
    const Function &function = m_program.functions[record.key.function];
    std::vector<State> inputs(function.blocks.size(), bottom());
    inputs[0] = record.entry;
    std::deque<Id> work{0};
    std::vector<bool> queued(function.blocks.size(), false);
    queued[0] = true;
    Summary output(m_program.objects.size());
    while (!work.empty()) {
      const Id b = work.front();
      work.pop_front();
      queued[b] = false;
      State state = inputs[b];
      for (const Instruction &i : function.blocks[b].instructions) {
        if (!state.reachable)
          break;
        State before = state;
        if (i.opcode == Opcode::Return) {
          Summary exit(m_program.objects.size());
          exit.reachable = true;
          exit.memory = state.memory;
          if (!i.operands.empty())
            exit.returned = eval(state, i.operands[0]);
          output.join(exit);
          record.snapshots.insert_or_assign(i.id,
                                            std::make_pair(before, state));
          state.reachable = false;
          break;
        }
        state = transfer(id, i, std::move(state));
        record.snapshots.insert_or_assign(
            i.id, std::make_pair(std::move(before), state));
      }
      if (!state.reachable)
        continue;
      for (const Edge &edge : function.blocks[b].successors) {
        State next = state;
        for (auto phi : edge.phi)
          if (m_slice.values[phi.first])
            next.values[phi.first] = eval(state, phi.second);
        if (inputs[edge.target].join(next) && !queued[edge.target]) {
          queued[edge.target] = true;
          work.push_back(edge.target);
        }
      }
    }
    return output;
  }

public:
  TabulationSolver(const Program &program, const ProgramIndex &index,
                   const Sets &upper, const CallGraphInfo &call_graph,
                   std::vector<Id> cluster_values, Slice slice,
                   const Options &options)
      : m_program(program), m_index(index), m_upper(upper),
        m_call_graph(call_graph), m_cluster_values(std::move(cluster_values)),
        m_slice(std::move(slice)), m_options(options) {
    m_stats.call_graph_sccs = m_call_graph.scc_count;
    m_stats.recursive_call_graph_sccs = m_call_graph.recursive_scc_count;
    m_stats.slice_values = m_slice.value_list.size();
    m_stats.slice_objects = m_slice.object_list.size();
    m_stats.slice_statements = m_slice.inst_list.size();
    try {
      Key root{program.entry,
               Sets(program.functions[program.entry].parameters.size()),
               Sets(program.objects.size())};
      const auto &parameters = program.functions[program.entry].parameters;
      for (std::size_t a = 0; a < parameters.size(); ++a)
        if (parameters[a] != INVALID && m_slice.values[parameters[a]])
          root.parameters[a] = PointsToSet::top();
      for (Id o : m_slice.object_list)
        if (program.objects[o].kind == ObjectKind::Global)
          root.memory[o] = program.objects[o].initial;
      context(std::move(root));
      while (!m_work.empty()) {
        const std::size_t id = takeScheduled();
        Summary next = evaluate(id);
        if (m_records[id]->summary.join(next)) {
          ++m_stats.summary_updates;
          for (std::size_t caller : m_records[id]->callers) {
            if (m_call_graph.function_scc[m_records[caller]->key.function] ==
                m_call_graph.function_scc[m_records[id]->key.function])
              ++m_stats.scc_reschedules;
            schedule(caller);
          }
        }
      }
    } catch (const LimitReached &) {
      m_failed = true;
    }
  }

  static bool memoryIntersects(const PointsToSet &a, const PointsToSet &b) {
    return a.intersectsNonNull(b);
  }

  std::set<std::size_t> collectActiveContexts(const Context *context_path,
                                              ContextMatch match) const {
    auto allReachable = [&]() {
      std::set<std::size_t> all{0};
      std::deque<std::size_t> work{0};
      while (!work.empty()) {
        const std::size_t id = work.front();
        work.pop_front();
        for (const auto &calls : m_records[id]->calls)
          for (std::size_t callee : calls.second)
            if (all.insert(callee).second)
              work.push_back(callee);
      }
      return all;
    };
    if (!context_path ||
        (match == ContextMatch::Suffix && context_path->empty()))
      return allReachable();

    std::set<std::size_t> active;
    if (match == ContextMatch::Full) {
      active.insert(0);
    } else {
      const Id root_fn = m_index.by_site[(*context_path)[0]].function;
      for (std::size_t id : allReachable())
        if (m_records[id]->key.function == root_fn)
          active.insert(id);
    }
    for (Id callsite : *context_path) {
      std::set<std::size_t> next;
      for (std::size_t id : active) {
        auto found = m_records[id]->calls.find(callsite);
        if (found != m_records[id]->calls.end())
          next.insert(found->second.begin(), found->second.end());
      }
      active = std::move(next);
    }
    return active;
  }

  QueryResult query(Id value, Id site, const Context *context_path, Point point,
                    ContextMatch match = ContextMatch::Full) override {
    if (m_failed)
      return {PointsToSet::top(), QueryStatus::ResourceLimit};
    std::set<std::size_t> active = collectActiveContexts(context_path, match);
    QueryResult result;
    for (std::size_t id : active) {
      auto found = m_records[id]->snapshots.find(site);
      if (found == m_records[id]->snapshots.end())
        continue;
      const State &state =
          point == Point::Before ? found->second.first : found->second.second;
      if (!state.reachable)
        continue;
      result.status = QueryStatus::Complete;
      PointsToSet set = eval(state, value);
      result.points_to.join(set.empty() ? PointsToSet::top() : set);
    }
    return result;
  }

  QueryResult queryObject(Id object, Id site, const Context *context_path,
                          Point point,
                          ContextMatch match = ContextMatch::Full) override {
    if (m_failed)
      return {PointsToSet::top(), QueryStatus::ResourceLimit};
    std::set<std::size_t> active = collectActiveContexts(context_path, match);
    QueryResult result;
    for (std::size_t id : active) {
      auto found = m_records[id]->snapshots.find(site);
      if (found == m_records[id]->snapshots.end())
        continue;
      const State &state =
          point == Point::Before ? found->second.first : found->second.second;
      if (!state.reachable)
        continue;
      result.status = QueryStatus::Complete;
      const PointsToSet &set = state.memory[object];
      result.points_to.join(set.empty() ? PointsToSet::top() : set);
    }
    return result;
  }

  bool isPointerMemObject(Id obj) const {
    if (obj < 2 || obj >= m_program.objects.size())
      return false;
    const Object &info = m_program.objects[obj];
    return isMemory(info) && (info.bytes > 0 || info.size_bytes == 0);
  }

  AliasResult aliasesFromBase(const QueryResult &base, Id self_val, Id self_obj,
                              Id site, const Context *context_path, Point point,
                              ContextMatch match) {
    AliasResult out;
    out.status = base.status;
    if (!base.reachable())
      return out;
    if (base.status == QueryStatus::ResourceLimit) {
      out.may_alias_all = true;
      return out;
    }
    if (base.points_to.isTop())
      out.may_alias_all = true;
    const Id fn = m_index.by_site[site].function;
    for (Id cand : m_cluster_values) {
      const Value &vinfo = m_program.values[cand];
      if (!vinfo.constant && vinfo.function != INVALID && vinfo.function != fn)
        continue;
      if (cand == self_val) {
        out.aliases.push_back(cand);
        continue;
      }
      QueryResult cq = query(cand, site, context_path, point, match);
      if (cq.reachable() && memoryIntersects(cq.points_to, base.points_to))
        out.aliases.push_back(cand);
    }

    const std::set<std::size_t> active =
        collectActiveContexts(context_path, match);
    const bool base_has_non_null =
        out.may_alias_all || memoryIntersects(base.points_to, base.points_to);
    for (Id cand_obj : m_slice.object_list) {
      if (!isPointerMemObject(cand_obj))
        continue;
      if (cand_obj == self_obj) {
        out.object_aliases.push_back(cand_obj);
        continue;
      }
      if (!base_has_non_null)
        continue;
      PointsToSet raw_mem;
      for (std::size_t id : active) {
        auto found = m_records[id]->snapshots.find(site);
        if (found == m_records[id]->snapshots.end())
          continue;
        const State &state =
            point == Point::Before ? found->second.first : found->second.second;
        if (!state.reachable)
          continue;
        raw_mem.join(state.memory[cand_obj]);
      }
      if (memoryIntersects(raw_mem, base.points_to))
        out.object_aliases.push_back(cand_obj);
    }
    if (self_obj != INVALID &&
        std::find(out.object_aliases.begin(), out.object_aliases.end(),
                  self_obj) == out.object_aliases.end())
      out.object_aliases.push_back(self_obj);
    return out;
  }

  AliasResult queryAliases(Id value, Id site, const Context *context_path,
                           Point point,
                           ContextMatch match = ContextMatch::Full) override {
    return aliasesFromBase(query(value, site, context_path, point, match),
                           value, INVALID, site, context_path, point, match);
  }

  AliasResult
  queryObjectAliases(Id object, Id site, const Context *context_path,
                     Point point,
                     ContextMatch match = ContextMatch::Full) override {
    return aliasesFromBase(
        queryObject(object, site, context_path, point, match), INVALID, object,
        site, context_path, point, match);
  }

  bool tracksObject(Id object) const override {
    return object < m_slice.objects.size() && m_slice.objects[object];
  }

  bool mustAlias(Id lhs, Id rhs, Id site, const Context *context_path,
                 ContextMatch match = ContextMatch::Full) override {
    QueryResult a = query(lhs, site, context_path, Point::Before, match);
    if (a.status != QueryStatus::Complete)
      return false;
    if (m_program.values[lhs].constant && m_program.values[lhs].initial.isTop())
      return false;
    if (lhs == rhs)
      return true;
    QueryResult b = query(rhs, site, context_path, Point::Before, match);
    if (b.status != QueryStatus::Complete)
      return false;
    if (m_program.values[rhs].constant && m_program.values[rhs].initial.isTop())
      return false;
    if (!memoryIntersects(a.points_to, b.points_to))
      return false;
    if (!a.points_to.isTop() && !b.points_to.isTop() &&
        a.points_to.size() == 1 && a.points_to == b.points_to) {
      const Id obj = *a.points_to.begin();
      return obj != NULL_OBJECT &&
             obj < m_index.exact_address_singleton.size() &&
             m_index.exact_address_singleton[obj];
    }
    return false;
  }

  const Statistics &statistics() const override { return m_stats; }
};

} // namespace

std::unique_ptr<ClusterSolver> createTabulationSolver(
    const Program &program, const ProgramIndex &index, const Sets &upper,
    const CallGraphInfo &call_graph, std::vector<Id> cluster_values,
    Slice slice, const Options &options) {
  return std::make_unique<TabulationSolver>(program, index, upper, call_graph,
                                            std::move(cluster_values),
                                            std::move(slice), options);
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
