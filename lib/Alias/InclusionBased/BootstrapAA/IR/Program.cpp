#include "Alias/InclusionBased/BootstrapAA/Engine.h"
#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace lotus {
namespace bootstrap {

using detail::nextId;

Program::Program() {
  Object unknown;
  unknown.name = "<unknown>";
  unknown.kind = ObjectKind::Unknown;
  objects.push_back(unknown);
  fields_of_base.emplace_back();
  Object null_object;
  null_object.name = "<null>";
  null_object.kind = ObjectKind::Null;
  objects.push_back(null_object);
  fields_of_base.emplace_back();
}

Id Program::addObject(Object object) {
  const Id id = nextId(objects.size(), "object");
  objects.push_back(std::move(object));
  fields_of_base.emplace_back();
  if (objects[id].field_sensitive && objects[id].base_object == INVALID)
    fields_of_base[id].emplace_back(objects[id].offset, id);
  return id;
}

Id Program::addFieldObject(Id base, std::uint64_t offset, Object object) {
  if (base >= objects.size())
    throw std::invalid_argument("BootstrapAA: invalid base object for field");
  objects[base].field_sensitive = true;
  if (fields_of_base[base].empty())
    fields_of_base[base].emplace_back(objects[base].offset, base);
  if (offset == objects[base].offset)
    return base;
  object.base_object = base;
  object.offset = offset;
  object.field_sensitive = true;
  const Id id = nextId(objects.size(), "object");
  objects.push_back(std::move(object));
  fields_of_base.emplace_back();
  auto &list = fields_of_base[base];
  auto pos = std::lower_bound(
      list.begin(), list.end(), offset,
      [](const auto &entry, std::uint64_t off) { return entry.first < off; });
  list.insert(pos, {offset, id});
  return id;
}

Id Program::addValue(Value value) {
  const Id id = nextId(values.size(), "value");
  values.push_back(std::move(value));
  return id;
}

Id Program::addConstant(PointsToSet points_to, std::string name) {
  Value value;
  value.name = std::move(name);
  value.constant = true;
  value.initial = std::move(points_to);
  return addValue(std::move(value));
}

Id Program::addFunction(std::string name, bool external) {
  const Id id = nextId(functions.size(), "function");
  Function function;
  function.name = std::move(name);
  function.external = external;
  functions.push_back(std::move(function));
  return id;
}

Id Program::addBlock(Id function) {
  auto &blocks = functions.at(function).blocks;
  const Id id = nextId(blocks.size(), "block");
  blocks.emplace_back();
  return id;
}

Id Program::append(Id function, Id block, Instruction instruction) {
  if (m_next_site == INVALID)
    throw std::length_error("BootstrapAA instruction ID space exhausted");
  instruction.id = m_next_site++;
  functions.at(function).blocks.at(block).instructions.push_back(instruction);
  return instruction.id;
}

Id Program::addCondition() {
  const Id id = nextId(cond_nodes.size(), "condition");
  cond_nodes.push_back({CondOp::Atom, id, INVALID});
  condition_count = static_cast<Id>(cond_nodes.size());
  return id;
}

Id Program::addCondNot(Id operand) {
  if (operand == INVALID)
    return INVALID;
  const Id id = nextId(cond_nodes.size(), "condition");
  cond_nodes.push_back({CondOp::Not, operand, INVALID});
  condition_count = static_cast<Id>(cond_nodes.size());
  return id;
}

Id Program::addCondAnd(Id lhs, Id rhs) {
  if (lhs == INVALID)
    return rhs;
  if (rhs == INVALID)
    return lhs;
  if (lhs == rhs)
    return lhs;
  const Id id = nextId(cond_nodes.size(), "condition");
  cond_nodes.push_back({CondOp::And, lhs, rhs});
  condition_count = static_cast<Id>(cond_nodes.size());
  return id;
}

Id Program::addCondOr(Id lhs, Id rhs) {
  if (lhs == INVALID || rhs == INVALID)
    return INVALID;
  if (lhs == rhs)
    return lhs;
  const Id id = nextId(cond_nodes.size(), "condition");
  cond_nodes.push_back({CondOp::Or, lhs, rhs});
  condition_count = static_cast<Id>(cond_nodes.size());
  return id;
}

Id Program::baseOf(Id object) const {
  if (object >= objects.size())
    return object;
  return objects[object].base_object == INVALID ? object
                                                : objects[object].base_object;
}

std::uint64_t Program::foldOffset(Id base, std::uint64_t offset) const {
  if (base >= objects.size())
    return offset;
  const auto &regions = objects[base].array_regions;
  // Fold nested array regions from outermost to innermost (regions are added
  // outermost-first).
  for (const ArrayRegion &r : regions) {
    if (r.stride > 0 && offset >= r.begin && offset < r.end)
      offset = r.begin + ((offset - r.begin) % r.stride);
  }
  return offset;
}

PointsToSet Program::resolveField(Id object, std::int64_t delta, bool known,
                                  std::uint64_t array_stride) const {
  if (object == UNKNOWN)
    return PointsToSet::top();
  if (object == NULL_OBJECT)
    return PointsToSet(NULL_OBJECT);
  if (object >= objects.size())
    return PointsToSet::top();

  const Id base = baseOf(object);
  const Object &base_info = objects[base];
  if (!base_info.field_sensitive || base >= fields_of_base.size() ||
      fields_of_base[base].empty()) {
    // Out-of-bounds or one-past-the-end offsets (e.g., `&g + 1`) on sized
    // non-field-sensitive objects must stay conservative so they never collapse
    // onto the same singleton object as `&g`.
    if (known && delta != 0 && base_info.size_bytes > 0) {
      if (delta > 0) {
        if (static_cast<std::uint64_t>(delta) >= base_info.size_bytes)
          return PointsToSet::top();
      } else {
        const std::uint64_t abs_d =
            static_cast<std::uint64_t>(-(delta + 1)) + 1;
        if (base_info.singleton || abs_d >= base_info.size_bytes)
          return PointsToSet::top();
      }
    }
    return PointsToSet(object);
  }

  const auto &fields = fields_of_base[base];
  auto allFieldsOfBase = [&]() -> PointsToSet {
    PointsToSet all;
    for (auto [off, fid] : fields)
      all.insert(fid);
    return all;
  };
  auto wholeAllocationPeriod = [&]() -> std::uint64_t {
    const std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t period = 0;
    for (const ArrayRegion &r : base_info.array_regions) {
      if (r.stride == 0)
        continue;
      if (period == 0) {
        if (r.begin == 0 &&
            (r.end == max_u64 ||
             (base_info.size_bytes > 0 && r.end == base_info.size_bytes)))
          period = r.stride;
      } else if (r.begin == 0 && r.end == period && (period % r.stride) == 0) {
        period = r.stride;
      }
    }
    return period;
  };
  auto modSignedOffset = [](std::int64_t cur_s, std::int64_t d,
                            std::uint64_t period) -> std::uint64_t {
    const std::uint64_t cmod = static_cast<std::uint64_t>(cur_s) % period;
    if (d >= 0) {
      const std::uint64_t dmod = static_cast<std::uint64_t>(d) % period;
      return (cmod + dmod) % period;
    }
    const std::uint64_t abs_d = static_cast<std::uint64_t>(-(d + 1)) + 1;
    const std::uint64_t dmod = abs_d % period;
    return (cmod + (period - dmod)) % period;
  };

  if (!known) {
    // Variable-index GEP inside an aggregate: fold modulo `array_stride` ONLY
    // when an effective whole-allocation folding period `period` covers
    // `[0, size_bytes)` (or an unbounded heap struct array `[0, max_u64)`) and
    // `period` divides `array_stride` (`array_stride % period == 0`).
    if (array_stride > 0) {
      const std::uint64_t period = wholeAllocationPeriod();
      const std::uint64_t cur_off = objects[object].offset;
      if (period > 0 && (array_stride % period) == 0 &&
          cur_off <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
        const std::int64_t cur_s = static_cast<std::int64_t>(cur_off);
        const std::uint64_t wrapped = modSignedOffset(cur_s, delta, period);
        const std::uint64_t folded = foldOffset(base, wrapped);
        auto it = std::lower_bound(
            fields.begin(), fields.end(), folded,
            [](const auto &e, std::uint64_t o) { return e.first < o; });
        if (it != fields.end() && it->first == folded)
          return PointsToSet(it->second);
      }
    }
    return allFieldsOfBase();
  }

  if (delta == 0)
    return PointsToSet(object);

  const std::uint64_t cur_off = objects[object].offset;
  if (cur_off > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
    return PointsToSet::top();
  const std::int64_t cur_s = static_cast<std::int64_t>(cur_off);
  if ((delta > 0 &&
       cur_s > std::numeric_limits<std::int64_t>::max() - delta) ||
      (delta < 0 && cur_s < std::numeric_limits<std::int64_t>::min() - delta))
    return PointsToSet::top();
  const std::int64_t target_signed = cur_s + delta;
  std::uint64_t target_raw = 0;
  if (target_signed < 0) {
    const std::uint64_t period = wholeAllocationPeriod();
    if (period > 0 && !base_info.singleton)
      target_raw = modSignedOffset(cur_s, delta, period);
    else
      return PointsToSet::top();
  } else {
    target_raw = static_cast<std::uint64_t>(target_signed);
  }
  if (base_info.size_bytes > 0 && target_raw >= base_info.size_bytes) {
    // One-past-the-end or out-of-bounds: keep conservative.
    return PointsToSet::top();
  }

  // If `object` lies inside a non-whole-allocation folded sub-array region and
  // `delta` can step across a stride boundary (e.g., `&s.arr[1] + 1` stepping
  // into sibling struct field `&s.tail`), widen soundly to all fields of `base`.
  if (!base_info.array_regions.empty()) {
    const std::uint64_t period = wholeAllocationPeriod();
    for (const ArrayRegion &r : base_info.array_regions) {
      if (r.stride == 0 || cur_off < r.begin || cur_off >= r.end)
        continue;
      const bool is_subarray = (period == 0 || r.stride < period);
      if (!is_subarray)
        continue;
      const std::uint64_t off_in_r = (cur_off - r.begin) % r.stride;
      const bool crosses_subarray =
          (delta < 0 &&
           static_cast<std::uint64_t>(-(delta + 1)) + 1 > off_in_r) ||
          (delta > 0 &&
           static_cast<std::uint64_t>(delta) >= r.stride - off_in_r);
      if (crosses_subarray)
        return allFieldsOfBase();
    }
  }

  const std::uint64_t folded = foldOffset(base, target_raw);
  auto it = std::lower_bound(
      fields.begin(), fields.end(), folded,
      [](const auto &e, std::uint64_t o) { return e.first < o; });
  if (it != fields.end() && it->first == folded)
    return PointsToSet(it->second);

  // Unaligned interior byte offset or padding inside `base`: widen to all
  // flattened fields of `base` so chained `Field` arithmetic from this interior
  // pointer (e.g., `(p + 1) + 15`) remains closed and never drops reachable
  // sibling fields.
  return allFieldsOfBase();
}

std::vector<Id> Program::overlappingFields(Id object,
                                           std::uint64_t bytes) const {
  if (object >= objects.size())
    return {};
  const Id base = baseOf(object);
  if (!objects[base].field_sensitive || base >= fields_of_base.size() ||
      fields_of_base[base].empty())
    return {object};

  const auto &fields = fields_of_base[base];
  if (bytes == 0) {
    std::vector<Id> all;
    all.reserve(fields.size());
    for (auto [off, fid] : fields)
      all.push_back(fid);
    return all;
  }

  auto fieldSpan = [&](std::size_t idx) -> std::uint64_t {
    const Id fid = fields[idx].second;
    if (idx == 0 && fields.size() > 1)
      return fields[1].first - fields[0].first;
    if (objects[fid].size_bytes != 0)
      return objects[fid].size_bytes;
    return objects[fid].bytes ? objects[fid].bytes : 1;
  };

  const std::uint64_t start = objects[object].offset;
  const std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t end = (bytes > max_u64 - start) ? max_u64 : start + bytes;

  // Track covered byte intervals [lo, hi), expanding across folded array
  // regions (both modulo-stride wraparound inside the region and spill past
  // the last element into trailing sibling struct fields).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals{{start, end}};
  auto addInterval = [&](std::uint64_t lo, std::uint64_t hi) {
    if (lo >= hi)
      return;
    for (auto &iv : intervals) {
      if (lo <= iv.second && hi >= iv.first) {
        iv.first = std::min(iv.first, lo);
        iv.second = std::max(iv.second, hi);
        return;
      }
    }
    intervals.emplace_back(lo, hi);
  };
  const auto &regions = objects[base].array_regions;
  if (!regions.empty()) {
    bool expanded = true;
    std::size_t guard_iters = 0;
    while (expanded && guard_iters++ < 8) {
      expanded = false;
      const std::size_t cur_n = intervals.size();
      for (std::size_t ii = 0; ii < cur_n; ++ii) {
        const auto [lo, hi] = intervals[ii];
        const std::uint64_t len = hi - lo;
        for (const ArrayRegion &r : regions) {
          if (r.stride == 0 || r.end <= r.begin)
            continue;
          if (hi <= r.begin || lo >= r.end)
            continue;
          const std::size_t before_sz = intervals.size();
          const auto before_copy = intervals;
          if (lo >= r.begin) {
            const std::uint64_t off_in_elem = (lo - r.begin) % r.stride;
            if (len >= r.stride) {
              addInterval(r.begin, r.begin + r.stride);
            } else if (off_in_elem + len > r.stride) {
              addInterval(r.begin + off_in_elem, r.begin + r.stride);
              addInterval(r.begin,
                          r.begin + (off_in_elem + len - r.stride));
            }
            if (r.end < max_u64) {
              const std::uint64_t num_elems = (r.end - r.begin) / r.stride;
              if (num_elems > 0) {
                const std::uint64_t last_start =
                    r.begin + (num_elems - 1) * r.stride + off_in_elem;
                const std::uint64_t last_end =
                    (len > max_u64 - last_start) ? max_u64 : last_start + len;
                if (last_end > r.end)
                  addInterval(r.end, last_end);
              }
            }
          } else {
            // Write starts before `r.begin` and enters `r`.
            const std::uint64_t in_len = hi - r.begin;
            if (in_len >= r.stride)
              addInterval(r.begin, r.begin + r.stride);
          }
          if (intervals.size() != before_sz || intervals != before_copy)
            expanded = true;
        }
      }
    }
  }

  std::vector<Id> result;
  for (std::size_t idx = 0; idx < fields.size(); ++idx) {
    const std::uint64_t off = fields[idx].first;
    const Id fid = fields[idx].second;
    const std::uint64_t span = fieldSpan(idx);
    const std::uint64_t fend = (span > max_u64 - off) ? max_u64 : off + span;
    bool hit = false;
    for (const auto &[lo, hi] : intervals) {
      if (off < hi && lo < fend) {
        hit = true;
        break;
      }
    }
    if (hit)
      result.push_back(fid);
  }
  if (result.empty())
    result.push_back(object);
  return result;
}

bool Program::memCopyRelativeOffsets(Id od, Id fd, std::uint64_t write_bytes,
                                     std::vector<std::int64_t> &rels) const {
  rels.clear();
  if (write_bytes == 0 || od >= objects.size() || fd >= objects.size())
    return false;
  const Object &fd_info = objects[fd];
  if (fd_info.bytes == 0)
    return false;
  const Id base = baseOf(od);
  if (baseOf(fd) != base)
    return false;

  const std::uint64_t od_off = objects[od].offset;
  const std::uint64_t fd_off = fd_info.offset;
  const auto &regions = objects[base].array_regions;
  const std::uint64_t max_i64 =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

  auto pushRel = [&](std::uint64_t r) -> bool {
    if (r > max_i64)
      return false;
    const std::int64_t sr = static_cast<std::int64_t>(r);
    if (std::find(rels.begin(), rels.end(), sr) == rels.end()) {
      if (rels.size() >= 16)
        return false;
      rels.push_back(sr);
    }
    return true;
  };

  if (regions.empty()) {
    if (fd_off < od_off)
      return false;
    const std::uint64_t rel = fd_off - od_off;
    if (rel > write_bytes || fd_info.bytes > write_bytes - rel)
      return false;
    return pushRel(rel);
  }

  std::vector<const ArrayRegion *> od_regs, fd_regs;
  for (const ArrayRegion &r : regions) {
    if (r.stride == 0 || r.end <= r.begin)
      continue;
    if (od_off >= r.begin && od_off < r.end)
      od_regs.push_back(&r);
    if (fd_off >= r.begin && fd_off < r.end)
      fd_regs.push_back(&r);
  }

  if (od_regs.empty() && fd_regs.empty()) {
    if (fd_off < od_off)
      return false;
    const std::uint64_t rel = fd_off - od_off;
    if (rel > write_bytes || fd_info.bytes > write_bytes - rel)
      return false;
    return pushRel(rel);
  }
  if (!od_regs.empty() && fd_regs.empty())
    return false;

  if (regions.size() == 1) {
    const ArrayRegion &r = regions[0];
    if (r.stride == 0 || r.end <= r.begin)
      return false;
    if (od_regs.empty()) {
      // `od` precedes `r`, `fd` is inside `r`.
      const std::uint64_t num_elems = (r.end - r.begin) / r.stride;
      const std::uint64_t fd_in = (fd_off - r.begin) % r.stride;
      const std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();
      const std::uint64_t write_end =
          (write_bytes > max_u64 - od_off) ? max_u64 : od_off + write_bytes;
      for (std::uint64_t k = 0; k < num_elems; ++k) {
        const std::uint64_t c = r.begin + k * r.stride + fd_in;
        if (c >= write_end)
          break;
        if (c < od_off || fd_info.bytes > write_end - c)
          return false;
        if (!pushRel(c - od_off))
          return false;
      }
      return !rels.empty();
    }
    // Both `od` and `fd` are inside the single region `r`.
    const std::uint64_t od_in = (od_off - r.begin) % r.stride;
    const std::uint64_t fd_in = (fd_off - r.begin) % r.stride;
    if (fd_in < od_in && od_in < fd_in + fd_info.bytes)
      return false;
    const std::uint64_t first_rel =
        (fd_in >= od_in) ? (fd_in - od_in) : (r.stride - (od_in - fd_in));
    for (std::uint64_t rel = first_rel; rel < write_bytes; rel += r.stride) {
      if (fd_info.bytes > write_bytes - rel)
        return false;
      if (!pushRel(rel))
        return false;
      if (r.stride > write_bytes - rel)
        break;
    }
    return !rels.empty();
  }

  // General nested multi-region enumeration (bounded to keep complexity O(1)).
  auto enumerateConcrete = [&](std::uint64_t folded_off,
                               std::vector<std::uint64_t> &out) -> bool {
    out = {folded_off};
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
      const ArrayRegion &r = *it;
      if (r.stride == 0 || r.end <= r.begin)
        continue;
      if (r.end == std::numeric_limits<std::uint64_t>::max())
        return false;
      const std::uint64_t num_elems = (r.end - r.begin) / r.stride;
      std::vector<std::uint64_t> next;
      for (std::uint64_t c : out) {
        if (c >= r.begin && c < r.end) {
          const std::uint64_t in_r = (c - r.begin) % r.stride;
          for (std::uint64_t k = 0; k < num_elems; ++k) {
            if (next.size() >= 16)
              return false;
            next.push_back(r.begin + k * r.stride + in_r);
          }
        } else {
          if (next.size() >= 16)
            return false;
          next.push_back(c);
        }
      }
      out = std::move(next);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return !out.empty();
  };

  std::vector<std::uint64_t> od_concs, fd_concs;
  if (!enumerateConcrete(od_off, od_concs) ||
      !enumerateConcrete(fd_off, fd_concs))
    return false;

  const std::uint64_t base_sz = objects[base].size_bytes;
  const std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();
  for (std::uint64_t oc : od_concs) {
    if (write_bytes > max_u64 - oc)
      return false;
    const std::uint64_t oend = oc + write_bytes;
    if (base_sz > 0 && oend > base_sz)
      continue;
    for (std::uint64_t fc : fd_concs) {
      if (fd_info.bytes > max_u64 - fc)
        return false;
      const std::uint64_t fend = fc + fd_info.bytes;
      if (fc < oend && oc < fend) {
        if (fc < oc || fend > oend)
          return false;
        if (!pushRel(fc - oc))
          return false;
      }
    }
  }
  return !rels.empty();
}

bool Program::zeroFillCoversField(Id od, Id fd,
                                  std::uint64_t write_bytes) const {
  if (write_bytes == 0 || od >= objects.size() || fd >= objects.size())
    return false;
  const Object &fd_info = objects[fd];
  if (fd_info.kind != ObjectKind::Global && fd_info.kind != ObjectKind::Stack &&
      fd_info.kind != ObjectKind::Heap)
    return false;
  const Id base = baseOf(od);
  if (baseOf(fd) != base)
    return false;
  const Object &base_info = objects[base];
  if (!base_info.field_sensitive || base >= fields_of_base.size() ||
      fields_of_base[base].empty()) {
    if (od != fd)
      return false;
    // Complete coverage of a single pointer cell (size_bytes == bytes > 0) or
    // the entire known-size allocation [0, size_bytes) zeroes every pointer in
    // it. Unknown-size allocations (size_bytes == 0) stay conservative.
    return fd_info.size_bytes > 0 && write_bytes >= fd_info.size_bytes;
  }
  if (fd_info.bytes > 0) {
    std::vector<std::int64_t> rels;
    return memCopyRelativeOffsets(od, fd, write_bytes, rels);
  }
  // Non-pointer flattened leaf field (bytes == 0, size_bytes > 0): any typed
  // pointer load already checks `info.bytes == read_bytes` and yields Top.
  return fd_info.size_bytes > 0;
}

void Program::validate() const {
  if (entry >= functions.size() || functions[entry].external)
    throw std::invalid_argument(
        "BootstrapAA requires a defined entry function");
  auto check_value = [&](Id value) {
    if (value != INVALID && value >= values.size())
      throw std::invalid_argument("invalid value ID");
  };
  auto check_set = [&](const PointsToSet &set) {
    for (Id object : set.objects())
      if (object >= objects.size())
        throw std::invalid_argument("invalid object ID in points-to set");
  };
  for (const Value &value : values) {
    check_set(value.initial);
    if (value.function != INVALID && value.function >= functions.size())
      throw std::invalid_argument("invalid value owner");
  }
  for (const Object &object : objects) {
    check_set(object.initial);
    if (object.kind == ObjectKind::Function &&
        object.function >= functions.size())
      throw std::invalid_argument("invalid function object");
    if (object.base_object != INVALID && object.base_object >= objects.size())
      throw std::invalid_argument("invalid field base object");
    if (object.singleton && object.kind != ObjectKind::Global &&
        object.kind != ObjectKind::Stack)
      throw std::invalid_argument(
          "only scalar globals/stacks may be singleton cells");
  }
  std::set<Id> sites;
  for (const Function &function : functions) {
    if (!function.external && function.blocks.empty())
      throw std::invalid_argument("defined function has no entry block");
    for (Id parameter : function.parameters)
      check_value(parameter);
    for (const Block &block : function.blocks) {
      for (const Edge &edge : block.successors) {
        if (edge.target >= function.blocks.size())
          throw std::invalid_argument("invalid CFG edge");
        for (auto phi : edge.phi) {
          check_value(phi.first);
          check_value(phi.second);
          if (phi.first == INVALID)
            throw std::invalid_argument("PHI has no destination");
        }
      }
      for (const Instruction &instruction : block.instructions) {
        if (instruction.id == INVALID || !sites.insert(instruction.id).second)
          throw std::invalid_argument(
              "instruction IDs must be unique (use append)");
        check_value(instruction.result);
        check_value(instruction.indirect_target);
        for (Id operand : instruction.operands)
          check_value(operand);
        if (instruction.callee != INVALID &&
            instruction.callee >= functions.size())
          throw std::invalid_argument("invalid callee ID");
        if ((instruction.opcode == Opcode::Address ||
             instruction.opcode == Opcode::Allocate) &&
            instruction.object >= objects.size())
          throw std::invalid_argument("invalid allocation object");
        if (((instruction.opcode == Opcode::Copy ||
              instruction.opcode == Opcode::Load ||
              instruction.opcode == Opcode::Field) &&
             instruction.operands.size() != 1) ||
            ((instruction.opcode == Opcode::Store ||
              instruction.opcode == Opcode::MemCopy) &&
             instruction.operands.size() != 2) ||
            (instruction.opcode == Opcode::Havoc &&
             instruction.operands.size() > 1) ||
            (instruction.opcode == Opcode::Return &&
             instruction.operands.size() > 1))
          throw std::invalid_argument("incorrect operand count");
        switch (instruction.opcode) {
        case Opcode::Address:
        case Opcode::Allocate:
        case Opcode::Copy:
        case Opcode::Join:
        case Opcode::Field:
        case Opcode::Load:
        case Opcode::Unknown:
          if (instruction.result == INVALID)
            throw std::invalid_argument(
                "pointer definition has no result at normalized instruction " +
                std::to_string(instruction.id) + " in function " +
                function.name + " (opcode " +
                std::to_string(static_cast<unsigned>(instruction.opcode)) +
                ")");
          break;
        default:
          break;
        }
      }
    }
  }
}

} // namespace bootstrap
} // namespace lotus
