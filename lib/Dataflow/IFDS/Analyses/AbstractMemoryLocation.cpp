#include "Dataflow/IFDS/Analyses/AbstractMemoryLocation.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <algorithm>
#include <cassert>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

namespace {
const std::vector<AbstractMemoryLocation::Offset> &empty_offsets() {
  static const std::vector<AbstractMemoryLocation::Offset> empty;
  return empty;
}
} // namespace

const llvm::Value *AbstractMemoryLocation::base() const noexcept {
  return m_node ? m_node->base : nullptr;
}

llvm::ArrayRef<AbstractMemoryLocation::Offset>
AbstractMemoryLocation::offsets() const noexcept {
  return m_node ? llvm::ArrayRef<Offset>(m_node->offsets) :
                  llvm::ArrayRef<Offset>(empty_offsets());
}

unsigned AbstractMemoryLocation::lifetime() const noexcept {
  return m_node ? m_node->lifetime : 0;
}

bool AbstractMemoryLocation::equivalent(
    const AbstractMemoryLocation &rhs) const noexcept {
  if (is_zero() || rhs.is_zero())
    return is_zero() && rhs.is_zero();
  if (base() != rhs.base())
    return false;
  const auto lhs_offsets = offsets();
  const auto rhs_offsets = rhs.offsets();
  const size_t common = std::min(lhs_offsets.size(), rhs_offsets.size());
  return lhs_offsets.take_front(common) == rhs_offsets.take_front(common);
}

bool AbstractMemoryLocation::equivalent_except_pointer_arithmetic(
    const AbstractMemoryLocation &rhs,
    unsigned pointer_arithmetic_levels) const noexcept {
  if (is_zero() || rhs.is_zero())
    return is_zero() && rhs.is_zero();
  if (base() != rhs.base())
    return false;
  const auto lhs_offsets = offsets();
  const auto rhs_offsets = rhs.offsets();
  const size_t common = std::min(lhs_offsets.size(), rhs_offsets.size());
  const size_t ignored = std::min<size_t>(pointer_arithmetic_levels, common);
  const size_t compared = common - ignored;
  return lhs_offsets.take_front(compared) == rhs_offsets.take_front(compared);
}

bool AbstractMemoryLocation::is_proper_prefix_of(
    const AbstractMemoryLocation &larger) const noexcept {
  if (is_zero() || larger.is_zero() || base() != larger.base())
    return false;
  const auto lhs = offsets();
  const auto rhs = larger.offsets();
  return lhs.size() < rhs.size() &&
         lhs == rhs.take_front(lhs.size());
}

bool AbstractMemoryLocation::must_alias(
    const AbstractMemoryLocation &rhs,
    const lotus::AliasAnalysisWrapper *alias) const {
  if (is_zero() || rhs.is_zero())
    return is_zero() && rhs.is_zero();
  if (base() != rhs.base()) {
    if (!alias || !alias->isInitialized() ||
        !const_cast<lotus::AliasAnalysisWrapper *>(alias)->mustAlias(
            base(), rhs.base()))
      return false;
  }
  // A bounded location is an over-approximation. Treat a common offset prefix
  // as a must-alias relation, matching the XTaint domain's prefix semantics.
  const auto lhs_offsets = offsets();
  const auto rhs_offsets = rhs.offsets();
  const size_t common = std::min(lhs_offsets.size(), rhs_offsets.size());
  return lhs_offsets.take_front(common) == rhs_offsets.take_front(common);
}

std::optional<AbstractMemoryLocation::Offset>
AbstractMemoryLocation::compute_offset(const llvm::DataLayout &layout,
                                       const llvm::GEPOperator *gep) {
  if (!gep)
    return std::nullopt;
  llvm::APInt offset(sizeof(Offset) * 8, 0, true);
  if (!gep->accumulateConstantOffset(layout, offset))
    return std::nullopt;
  return offset.getSExtValue();
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const AbstractMemoryLocation &location) {
  os << "(";
  if (location.is_zero()) {
    os << "<ZERO>";
  } else {
    if (location.base())
      location.base()->print(os);
    else
      os << "<null>";
  }
  os << "; offsets=[";
  bool first = true;
  for (auto offset : location.offsets()) {
    if (!first)
      os << ", ";
    first = false;
    os << offset;
  }
  os << "] #" << location.lifetime() << ")";
  return os;
}

bool AbstractMemoryLocationFactory::Key::operator<(
    const Key &rhs) const noexcept {
  if (base != rhs.base)
    return base < rhs.base;
  if (offsets != rhs.offsets)
    return std::lexicographical_compare(offsets.begin(), offsets.end(),
                                        rhs.offsets.begin(), rhs.offsets.end());
  return lifetime < rhs.lifetime;
}

AbstractMemoryLocationFactory::AbstractMemoryLocationFactory(
    const llvm::DataLayout *layout, size_t reserve)
    : m_layout(layout) {
  (void)reserve;
}

AbstractMemoryLocation AbstractMemoryLocationFactory::intern(
    const llvm::Value *base, llvm::ArrayRef<Offset> offsets,
    unsigned lifetime) {
  if (!base)
    return zero();
  Key key;
  key.base = base;
  key.offsets.assign(offsets.begin(), offsets.end());
  key.lifetime = lifetime;
  auto it = m_nodes.find(key);
  if (it != m_nodes.end())
    return AbstractMemoryLocation(it->second);
  auto node = std::make_shared<const AbstractMemoryLocation::Node>(
      AbstractMemoryLocation::Node{base, key.offsets, lifetime});
  auto inserted = m_nodes.emplace(std::move(key), std::move(node));
  return AbstractMemoryLocation(inserted.first->second);
}

AbstractMemoryLocation AbstractMemoryLocationFactory::create_impl(
    const llvm::Value *value, unsigned bound) {
  if (!value)
    return zero();
  if (!m_layout)
    return intern(value, {0}, bound == 0 ? 0 : bound - 1);

  // Walk the def-use chain in reverse. Loads add an indirection (represented
  // by a zero offset), casts are transparent, and constant GEPs contribute a
  // byte offset. A dynamic GEP intentionally collapses the path to a bounded
  // base location, which is the sound over-approximation.
  // Every location starts with one offset for the base value.  Loads append
  // another component, so a load from `%p` is represented as `[0, 0]` while
  // `%p` itself is `[0]`.  Keeping that initial component is what preserves
  // the distinction between a pointer value and the memory reached through
  // it when the path is later extended with field offsets.
  std::vector<Offset> offsets{0};
  const llvm::Value *base = value;
  unsigned versions = 1;
  while (true) {
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(base)) {
      offsets.push_back(0);
      ++versions;
      base = load->getPointerOperand();
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(base)) {
      base = cast->getOperand(0);
      continue;
    }
    if (const auto *gep = llvm::dyn_cast<llvm::GEPOperator>(base)) {
      auto offset = AbstractMemoryLocation::compute_offset(*m_layout, gep);
      if (!offset) {
        offsets.clear();
        offsets.push_back(0);
        versions = bound;
      } else if (offsets.empty()) {
        offsets.push_back(*offset);
      } else {
        offsets.back() += *offset;
      }
      base = gep->getPointerOperand();
      continue;
    }
    break;
  }
  std::reverse(offsets.begin(), offsets.end());
  // The lifetime is the number of additional load/field steps that remain
  // precise.
  unsigned lifetime = bound > std::min(versions, bound)
                           ? bound - std::min(versions, bound)
                           : 0;
  if (offsets.size() > bound && bound != 0) {
    offsets.resize(bound);
    lifetime = 0;
  }
  return intern(base, offsets, lifetime);
}

AbstractMemoryLocation AbstractMemoryLocationFactory::create(
    const llvm::Value *value, unsigned bound) {
  return create_impl(value, bound);
}

AbstractMemoryLocation AbstractMemoryLocationFactory::limit(
    const AbstractMemoryLocation &location) {
  if (location.is_zero())
    return location;
  auto offsets = location.offsets();
  if (!offsets.empty())
    offsets = offsets.drop_back();
  return intern(location.base(), offsets, 0);
}

AbstractMemoryLocation AbstractMemoryLocationFactory::with_indirection(
    const AbstractMemoryLocation &location, llvm::ArrayRef<Offset> extra) {
  if (location.is_zero() || location.lifetime() == 0)
    return location;
  auto old = location.offsets();
  std::vector<Offset> offsets(old.begin(), old.end());
  if (extra.empty()) {
    offsets.push_back(0);
  } else {
    const size_t count = std::min<size_t>(location.lifetime(), extra.size());
    offsets.insert(offsets.end(), extra.begin(), extra.begin() + count);
  }
  const unsigned consumed = extra.empty() ? 1u
                                           : static_cast<unsigned>(
                                                 std::min<size_t>(
                                                     location.lifetime(),
                                                     extra.size()));
  return intern(location.base(), offsets, location.lifetime() - consumed);
}

AbstractMemoryLocation AbstractMemoryLocationFactory::with_offset(
    const AbstractMemoryLocation &location, const llvm::GEPOperator *gep) {
  if (location.is_zero() || location.lifetime() == 0)
    return location;
  if (!m_layout)
    return limit(location);
  auto offset = AbstractMemoryLocation::compute_offset(*m_layout, gep);
  if (!offset)
    return limit(location);
  auto offsets = location.offsets();
  if (offsets.empty())
    return location;
  std::vector<Offset> next(offsets.begin(), offsets.end());
  next.back() += *offset;
  return intern(location.base(), next, location.lifetime());
}

AbstractMemoryLocation AbstractMemoryLocationFactory::with_offsets(
    const AbstractMemoryLocation &location, llvm::ArrayRef<Offset> extra) {
  if (location.is_zero() || location.lifetime() == 0 || extra.empty())
    return location;
  auto offsets = location.offsets();
  if (offsets.empty())
    return location;
  const size_t count = std::min<size_t>(location.lifetime(), extra.size());
  std::vector<Offset> next(offsets.begin(), offsets.end());
  next.back() += extra.front();
  next.insert(next.end(), extra.begin() + 1, extra.begin() + count);
  return intern(location.base(), next,
                location.lifetime() - static_cast<unsigned>(count) + 1);
}

AbstractMemoryLocation AbstractMemoryLocationFactory::with_transfer_to(
    const AbstractMemoryLocation &location,
    const AbstractMemoryLocation &from, const llvm::Value *to) {
  if (!to)
    return zero();
  if (location.is_zero() || from.is_zero())
    return intern(to, {}, 0);
  auto lhs = location.offsets();
  auto rhs = from.offsets();
  auto larger = lhs.size() >= rhs.size() ? lhs : rhs;
  auto smaller = lhs.size() >= rhs.size() ? rhs : lhs;
  size_t drop = smaller.empty() ? 0 : smaller.size() - 1;
  if (drop < larger.size())
    larger = larger.drop_front(drop);
  std::vector<Offset> offsets(larger.begin(), larger.end());
  if (!offsets.empty())
    offsets.back() = 0;
  return intern(to, offsets, std::min(location.lifetime(), from.lifetime()));
}

AbstractMemoryLocation AbstractMemoryLocationFactory::with_transfer_from(
    const AbstractMemoryLocation &location,
    const AbstractMemoryLocation &to) {
  if (location.is_zero() || to.is_zero())
    return to;
  auto lhs = location.offsets();
  auto rhs = to.offsets();
  std::vector<Offset> offsets(rhs.begin(), rhs.end());
  if (!lhs.empty()) {
    if (!offsets.empty())
      offsets.back() += lhs.front();
    offsets.insert(offsets.end(), lhs.begin() + 1, lhs.end());
  }
  const size_t max_size = std::min(lhs.size() + location.lifetime(),
                                   rhs.size() + to.lifetime());
  if (offsets.size() > max_size)
    offsets.resize(max_size);
  const unsigned lifetime = static_cast<unsigned>(
      std::min<size_t>(location.lifetime(), max_size - offsets.size()));
  return intern(to.base(), offsets, lifetime);
}

} // namespace ifds
