#pragma once

// A compact, internable abstraction for the memory locations used by the
// extended taint analyses. The representation follows the useful part of
// PhASAR's XTaint location model while keeping Lotus's C++17/LLVM 14 API: a
// base LLVM value, a sequence of constant byte offsets, and a lifetime bound.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/DataLayout.h>

namespace llvm {
class GEPOperator;
class Value;
class raw_ostream;
} // namespace llvm

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace ifds {

class AbstractMemoryLocation {
public:
  using Offset = std::ptrdiff_t;

  AbstractMemoryLocation() = default;

  bool is_zero() const noexcept { return !m_node; }
  bool isZero() const noexcept { return is_zero(); }
  const llvm::Value *base() const noexcept;
  llvm::ArrayRef<Offset> offsets() const noexcept;
  unsigned lifetime() const noexcept;
  bool is_over_approximation() const noexcept { return lifetime() == 0; }
  bool isOverApproximation() const noexcept { return is_over_approximation(); }

  friend bool operator==(const AbstractMemoryLocation &lhs,
                         const AbstractMemoryLocation &rhs) noexcept {
    return lhs.m_node == rhs.m_node;
  }
  friend bool operator!=(const AbstractMemoryLocation &lhs,
                         const AbstractMemoryLocation &rhs) noexcept {
    return !(lhs == rhs);
  }
  bool operator<(const AbstractMemoryLocation &rhs) const noexcept {
    return m_node.get() < rhs.m_node.get();
  }

  // Prefix-equivalence deliberately treats a bounded location as an
  // over-approximation of a more precise descendant.
  bool equivalent(const AbstractMemoryLocation &rhs) const noexcept;
  bool equivalent_except_pointer_arithmetic(
      const AbstractMemoryLocation &rhs,
      unsigned pointer_arithmetic_levels = 1) const noexcept;
  bool equivalentExceptPointerArithmetics(
      const AbstractMemoryLocation &rhs,
      unsigned pointer_arithmetic_levels = 1) const noexcept {
    return equivalent_except_pointer_arithmetic(rhs, pointer_arithmetic_levels);
  }
  bool is_proper_prefix_of(const AbstractMemoryLocation &larger) const noexcept;
  bool isProperPrefixOf(const AbstractMemoryLocation &larger) const noexcept {
    return is_proper_prefix_of(larger);
  }
  bool must_alias(const AbstractMemoryLocation &rhs,
                  const lotus::AliasAnalysisWrapper *alias) const;
  bool mustAlias(const AbstractMemoryLocation &rhs,
                 const lotus::AliasAnalysisWrapper *alias) const {
    return must_alias(rhs, alias);
  }

  static std::optional<Offset>
  compute_offset(const llvm::DataLayout &layout, const llvm::GEPOperator *gep);

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                       const AbstractMemoryLocation &location);

private:
  struct Node {
    const llvm::Value *base = nullptr;
    std::vector<Offset> offsets;
    unsigned lifetime = 0;
  };

  explicit AbstractMemoryLocation(std::shared_ptr<const Node> node)
      : m_node(std::move(node)) {}

  std::shared_ptr<const Node> m_node;
  friend class AbstractMemoryLocationFactory;
};

class AbstractMemoryLocationFactory {
public:
  using Offset = AbstractMemoryLocation::Offset;

  explicit AbstractMemoryLocationFactory(const llvm::DataLayout *layout = nullptr,
                                         size_t reserve = 0);
  AbstractMemoryLocationFactory(const AbstractMemoryLocationFactory &) = delete;
  AbstractMemoryLocationFactory &operator=(
      const AbstractMemoryLocationFactory &) = delete;

  void set_data_layout(const llvm::DataLayout &layout) {
    if (m_layout == &layout)
      return;
    // A factory is tied to the module's DataLayout.  Drop interned nodes when
    // a client reuses the analysis for another module so field offsets from a
    // previous target cannot be mixed into the new fact domain.
    m_nodes.clear();
    m_layout = &layout;
  }
  const llvm::DataLayout *data_layout() const noexcept { return m_layout; }

  AbstractMemoryLocation zero() const { return {}; }
  AbstractMemoryLocation create(const llvm::Value *value, unsigned bound);
  AbstractMemoryLocation limit(const AbstractMemoryLocation &location);
  AbstractMemoryLocation with_indirection(
      const AbstractMemoryLocation &location,
      llvm::ArrayRef<Offset> offsets = {});
  AbstractMemoryLocation withIndirectionOf(
      const AbstractMemoryLocation &location,
      llvm::ArrayRef<Offset> offsets = {}) {
    return with_indirection(location, offsets);
  }
  AbstractMemoryLocation with_offset(const AbstractMemoryLocation &location,
                                     const llvm::GEPOperator *gep);
  AbstractMemoryLocation withOffset(const AbstractMemoryLocation &location,
                                    const llvm::GEPOperator *gep) {
    return with_offset(location, gep);
  }
  AbstractMemoryLocation with_offsets(const AbstractMemoryLocation &location,
                                      llvm::ArrayRef<Offset> offsets);
  AbstractMemoryLocation withOffsets(const AbstractMemoryLocation &location,
                                     llvm::ArrayRef<Offset> offsets) {
    return with_offsets(location, offsets);
  }
  AbstractMemoryLocation with_transfer_to(const AbstractMemoryLocation &location,
                                          const AbstractMemoryLocation &from,
                                          const llvm::Value *to);
  AbstractMemoryLocation withTransferTo(const AbstractMemoryLocation &location,
                                        const AbstractMemoryLocation &from,
                                        const llvm::Value *to) {
    return with_transfer_to(location, from, to);
  }
  AbstractMemoryLocation with_transfer_from(
      const AbstractMemoryLocation &location,
      const AbstractMemoryLocation &to);
  AbstractMemoryLocation withTransferFrom(
      const AbstractMemoryLocation &location,
      const AbstractMemoryLocation &to) {
    return with_transfer_from(location, to);
  }

  size_t size() const noexcept { return m_nodes.size(); }

private:
  struct Key {
    const llvm::Value *base = nullptr;
    std::vector<Offset> offsets;
    unsigned lifetime = 0;
    bool operator<(const Key &rhs) const noexcept;
  };

  AbstractMemoryLocation intern(const llvm::Value *base,
                                llvm::ArrayRef<Offset> offsets,
                                unsigned lifetime);
  AbstractMemoryLocation create_impl(const llvm::Value *value,
                                     unsigned bound);

  const llvm::DataLayout *m_layout = nullptr;
  std::map<Key, std::shared_ptr<const AbstractMemoryLocation::Node>> m_nodes;
};

} // namespace ifds

namespace std {
template <> struct hash<ifds::AbstractMemoryLocation> {
  size_t operator()(const ifds::AbstractMemoryLocation &location) const
      noexcept {
    size_t h = std::hash<const llvm::Value *>{}(location.base());
    h ^= std::hash<unsigned>{}(location.lifetime()) +
         static_cast<size_t>(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
    for (auto offset : location.offsets()) {
      const auto value = static_cast<std::uint64_t>(offset);
      h ^= std::hash<std::uint64_t>{}(value) +
           static_cast<size_t>(0x9e3779b97f4a7c15ULL) + (h << 6) +
           (h >> 2);
    }
    return h;
  }
};
} // namespace std
