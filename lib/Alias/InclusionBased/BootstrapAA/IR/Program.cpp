#include "Alias/InclusionBased/BootstrapAA/Engine.h"
#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"

#include <algorithm>
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
  if (condition_count == INVALID)
    throw std::length_error("BootstrapAA condition ID space exhausted");
  return condition_count++;
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
    // Field-insensitive / scalar / untyped allocation: inbounds field offset
    // stays within the same abstract object.
    return PointsToSet(object);
  }

  const auto &fields = fields_of_base[base];
  if (!known) {
    // Variable-index GEP inside an aggregate: if indexing within a known array
    // region of matching stride, fold to the representative element offset;
    // otherwise conservatively include all flattened fields of `base`.
    if (array_stride > 0) {
      const std::uint64_t cur_off = objects[object].offset;
      for (const ArrayRegion &r : base_info.array_regions) {
        if (r.stride == array_stride && cur_off >= r.begin &&
            cur_off < r.end) {
          const std::int64_t target_signed =
              static_cast<std::int64_t>(cur_off) + delta;
          if (target_signed >= 0) {
            const std::uint64_t folded =
                foldOffset(base, static_cast<std::uint64_t>(target_signed));
            auto it = std::lower_bound(
                fields.begin(), fields.end(), folded,
                [](const auto &e, std::uint64_t o) { return e.first < o; });
            if (it != fields.end() && it->first == folded)
              return PointsToSet(it->second);
          }
        }
      }
    }
    PointsToSet all;
    for (auto [off, fid] : fields)
      all.insert(fid);
    return all;
  }

  if (delta == 0)
    return PointsToSet(object);

  const std::int64_t target_signed =
      static_cast<std::int64_t>(objects[object].offset) + delta;
  if (target_signed < 0)
    return PointsToSet::top();
  const std::uint64_t target_raw = static_cast<std::uint64_t>(target_signed);
  if (base_info.size_bytes > 0 && target_raw >= base_info.size_bytes) {
    // One-past-the-end or out-of-bounds: keep conservative.
    return PointsToSet::top();
  }
  const std::uint64_t folded = foldOffset(base, target_raw);
  auto it = std::lower_bound(
      fields.begin(), fields.end(), folded,
      [](const auto &e, std::uint64_t o) { return e.first < o; });
  if (it != fields.end() && it->first == folded)
    return PointsToSet(it->second);

  // Offset falls inside an existing field (partial / unaligned byte access) or
  // padding: return overlapping field(s) if any, else conservative top.
  // For the base object at index 0, `size_bytes` records total aggregate size,
  // so its individual field span stops at the next field's offset.
  auto fieldSpan = [&](std::size_t idx) -> std::uint64_t {
    const Id fid = fields[idx].second;
    if (idx == 0 && fields.size() > 1)
      return fields[1].first - fields[0].first;
    if (objects[fid].size_bytes != 0)
      return objects[fid].size_bytes;
    return objects[fid].bytes;
  };
  PointsToSet overlap;
  for (std::size_t idx = 0; idx < fields.size(); ++idx) {
    const std::uint64_t off = fields[idx].first;
    const Id fid = fields[idx].second;
    const std::uint64_t span = fieldSpan(idx);
    if (span > 0 && folded >= off && folded < off + span)
      overlap.insert(fid);
  }
  return overlap.empty() ? PointsToSet::top() : overlap;
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
  const std::uint64_t end = start + bytes;
  std::vector<Id> result;
  for (std::size_t idx = 0; idx < fields.size(); ++idx) {
    const std::uint64_t off = fields[idx].first;
    const Id fid = fields[idx].second;
    const std::uint64_t span = fieldSpan(idx);
    const std::uint64_t fend = off + span;
    if (off < end && start < fend)
      result.push_back(fid);
  }
  if (result.empty())
    result.push_back(object);
  return result;
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
