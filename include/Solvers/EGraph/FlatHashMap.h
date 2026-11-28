#pragma once

#include "Solvers/EGraph/Id.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define LOTUS_EGRAPH_SWISS_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define LOTUS_EGRAPH_SWISS_SSE2 1
#endif

namespace lotus::egraph {

inline uint64_t fastMix64(uint64_t x) noexcept {
#if defined(__SIZEOF_INT128__)
  __uint128_t r = static_cast<__uint128_t>(x) * 0x9e3779b97f4a7c15ULL;
  return static_cast<uint64_t>(r ^ (r >> 64));
#else
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
#endif
}

inline uint64_t fxHashWord(uint64_t seed, uint64_t word) noexcept {
  return (seed * 0x517cc1b727220a95ULL) ^ word;
}

namespace detail {

inline constexpr int8_t kSwissEmpty = -128;
inline constexpr int8_t kSwissDeleted = -2;

inline int8_t swissH2(uint64_t hash) noexcept {
  return static_cast<int8_t>(hash >> 57);
}

inline bool swissIsFull(int8_t c) noexcept { return c >= 0; }
inline bool swissIsEmpty(int8_t c) noexcept { return c == kSwissEmpty; }

#if defined(LOTUS_EGRAPH_SWISS_NEON)

struct SwissGroup {
  static constexpr size_t kWidth = 16;
  using Mask = uint64_t;

  int8x16_t ctrl;

  explicit SwissGroup(const int8_t *pos) noexcept : ctrl(vld1q_s8(pos)) {}

  static inline Mask toMask(uint8x16_t cmp) noexcept {
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(cmp), 4);
    return vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
  }

  inline Mask match(int8_t h2) const noexcept {
    return toMask(vceqq_s8(ctrl, vdupq_n_s8(h2)));
  }

  inline Mask matchEmpty() const noexcept {
    return toMask(vceqq_s8(ctrl, vdupq_n_s8(kSwissEmpty)));
  }

  inline Mask matchEmptyOrDeleted() const noexcept {
    return toMask(vcltzq_s8(ctrl));
  }

  static inline unsigned lowestBit(Mask m) noexcept {
    return static_cast<unsigned>(__builtin_ctzll(m)) >> 2;
  }

  static inline unsigned highestBit(Mask m) noexcept {
    return static_cast<unsigned>(63 - __builtin_clzll(m)) >> 2;
  }

  static inline Mask clearLowestBit(Mask m, unsigned lane) noexcept {
    return m ^ (0xFULL << (lane << 2));
  }
};

#elif defined(LOTUS_EGRAPH_SWISS_SSE2)

struct SwissGroup {
  static constexpr size_t kWidth = 16;
  using Mask = uint32_t;

  __m128i ctrl;

  explicit SwissGroup(const int8_t *pos) noexcept
      : ctrl(_mm_loadu_si128(reinterpret_cast<const __m128i *>(pos))) {}

  inline Mask match(int8_t h2) const noexcept {
    return static_cast<Mask>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(h2))));
  }

  inline Mask matchEmpty() const noexcept {
    return static_cast<Mask>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(kSwissEmpty))));
  }

  inline Mask matchEmptyOrDeleted() const noexcept {
    return static_cast<Mask>(_mm_movemask_epi8(ctrl));
  }

  static inline unsigned lowestBit(Mask m) noexcept {
    return static_cast<unsigned>(__builtin_ctz(m));
  }

  static inline unsigned highestBit(Mask m) noexcept {
    return static_cast<unsigned>(31 - __builtin_clz(m));
  }

  static inline Mask clearLowestBit(Mask m, unsigned) noexcept {
    return m & (m - 1);
  }
};

#else

struct SwissGroup {
  static constexpr size_t kWidth = 8;
  using Mask = uint64_t;

  uint64_t ctrl;

  explicit SwissGroup(const int8_t *pos) noexcept {
    std::memcpy(&ctrl, pos, sizeof(ctrl));
  }

  inline Mask match(int8_t h2) const noexcept {
    constexpr uint64_t kLsbs = 0x0101010101010101ULL;
    constexpr uint64_t kMsbs = 0x8080808080808080ULL;
    uint64_t x = ctrl ^ (kLsbs * static_cast<uint8_t>(h2));
    return (x - kLsbs) & ~x & kMsbs;
  }

  inline Mask matchEmpty() const noexcept {
    constexpr uint64_t kMsbs = 0x8080808080808080ULL;
    return ctrl & (~ctrl << 6) & kMsbs;
  }

  inline Mask matchEmptyOrDeleted() const noexcept {
    constexpr uint64_t kMsbs = 0x8080808080808080ULL;
    return ctrl & kMsbs;
  }

  static inline unsigned lowestBit(Mask m) noexcept {
    return static_cast<unsigned>(__builtin_ctzll(m)) >> 3;
  }

  static inline unsigned highestBit(Mask m) noexcept {
    return static_cast<unsigned>(63 - __builtin_clzll(m)) >> 3;
  }

  static inline Mask clearLowestBit(Mask m, unsigned) noexcept {
    return m & (m - 1);
  }
};

#endif

inline size_t swissNextPow2(size_t n) noexcept {
  size_t cap = SwissGroup::kWidth;
  while (cap < n) {
    cap <<= 1;
  }
  return cap;
}

inline size_t swissCapacityToGrowth(size_t cap) noexcept {
  // 7/8 max load factor (0.875), standard SwissTable threshold for high density
  // and minimal L2/SLC cache footprint.
  return (cap * 7) >> 3;
}

inline size_t swissSizeToCapacity(size_t count) noexcept {
  if (count == 0) {
    return 0;
  }
  size_t needed = (count * 8 + 6) / 7;
  return swissNextPow2(std::max<size_t>(SwissGroup::kWidth, needed));
}

} // namespace detail

// SwissTable open-addressing hash map supporting non-default-constructible
// keys and values (e.g. LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE nodes).
template <typename K, typename V, typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class FlatHashMap {
public:
  using key_type = K;
  using mapped_type = V;
  using value_type = std::pair<const K, V>;
  using size_type = size_t;
  using hasher = Hash;
  using key_equal = KeyEqual;

private:
  using MutablePair = std::pair<K, V>;

  union Slot {
    Slot() noexcept {}
    ~Slot() noexcept {}
    value_type kv;
    MutablePair mutable_kv;
  };

public:
  template <bool IsConst> class IteratorBase {
  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::conditional_t<IsConst, const std::pair<const K, V>,
                                          std::pair<const K, V>>;
    using pointer = value_type *;
    using reference = value_type &;

    IteratorBase() = default;

    template <bool OtherConst,
              typename = std::enable_if_t<IsConst && !OtherConst>>
    IteratorBase(const IteratorBase<OtherConst> &other) noexcept
        : ctrl_(other.ctrl_), slots_(other.slots_), idx_(other.idx_),
          cap_(other.cap_) {}

    reference operator*() const noexcept { return slots_[idx_].kv; }
    pointer operator->() const noexcept { return &slots_[idx_].kv; }

    IteratorBase &operator++() noexcept {
      ++idx_;
      skipEmpty();
      return *this;
    }

    IteratorBase operator++(int) noexcept {
      IteratorBase tmp = *this;
      ++(*this);
      return tmp;
    }

    friend bool operator==(const IteratorBase &a,
                           const IteratorBase &b) noexcept {
      return a.idx_ == b.idx_ && a.ctrl_ == b.ctrl_;
    }

    friend bool operator!=(const IteratorBase &a,
                           const IteratorBase &b) noexcept {
      return !(a == b);
    }

  private:
    friend class FlatHashMap;
    template <bool> friend class IteratorBase;

    using SlotPtr = std::conditional_t<IsConst, const Slot *, Slot *>;

    IteratorBase(const int8_t *ctrl, SlotPtr slots, size_t idx,
                 size_t cap) noexcept
        : ctrl_(ctrl), slots_(slots), idx_(idx), cap_(cap) {
      skipEmpty();
    }

    IteratorBase(const int8_t *ctrl, SlotPtr slots, size_t idx, size_t cap,
                 int) noexcept
        : ctrl_(ctrl), slots_(slots), idx_(idx), cap_(cap) {}

    void skipEmpty() noexcept {
      while (idx_ < cap_ && !detail::swissIsFull(ctrl_[idx_])) {
        ++idx_;
      }
    }

    const int8_t *ctrl_ = nullptr;
    SlotPtr slots_ = nullptr;
    size_t idx_ = 0;
    size_t cap_ = 0;
  };

  using iterator = IteratorBase<false>;
  using const_iterator = IteratorBase<true>;

  FlatHashMap() = default;
  explicit FlatHashMap(size_t initial_count) { reserve(initial_count); }

  FlatHashMap(const FlatHashMap &other)
      : hasher_(other.hasher_), equal_(other.equal_) {
    if (other.size_ == 0) {
      return;
    }
    allocate(other.cap_);
    for (size_t i = 0; i < other.cap_; ++i) {
      if (detail::swissIsFull(other.ctrl_[i])) {
        setCtrl(i, other.ctrl_[i]);
        ::new (&slots_[i].kv) value_type(other.slots_[i].kv);
      }
    }
    size_ = other.size_;
    growth_left_ = detail::swissCapacityToGrowth(cap_) - size_;
  }

  FlatHashMap(FlatHashMap &&other) noexcept
      : ctrl_(other.ctrl_), slots_(other.slots_), cap_(other.cap_),
        size_(other.size_), growth_left_(other.growth_left_),
        hasher_(std::move(other.hasher_)), equal_(std::move(other.equal_)) {
    other.ctrl_ = nullptr;
    other.slots_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.growth_left_ = 0;
  }

  FlatHashMap &operator=(const FlatHashMap &other) {
    if (this == &other) {
      return *this;
    }
    FlatHashMap tmp(other);
    swap(tmp);
    return *this;
  }

  FlatHashMap &operator=(FlatHashMap &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    destroyAll();
    deallocate();
    ctrl_ = other.ctrl_;
    slots_ = other.slots_;
    cap_ = other.cap_;
    size_ = other.size_;
    growth_left_ = other.growth_left_;
    hasher_ = std::move(other.hasher_);
    equal_ = std::move(other.equal_);
    other.ctrl_ = nullptr;
    other.slots_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.growth_left_ = 0;
    return *this;
  }

  ~FlatHashMap() {
    destroyAll();
    deallocate();
  }

  void swap(FlatHashMap &other) noexcept {
    std::swap(ctrl_, other.ctrl_);
    std::swap(slots_, other.slots_);
    std::swap(cap_, other.cap_);
    std::swap(size_, other.size_);
    std::swap(growth_left_, other.growth_left_);
    std::swap(hasher_, other.hasher_);
    std::swap(equal_, other.equal_);
  }

  bool empty() const noexcept { return size_ == 0; }
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return cap_; }
  void max_load_factor(float) noexcept {}

  iterator begin() noexcept { return iterator(ctrl_, slots_, 0, cap_); }
  iterator end() noexcept { return iterator(ctrl_, slots_, cap_, cap_, 0); }
  const_iterator begin() const noexcept {
    return const_iterator(ctrl_, slots_, 0, cap_);
  }
  const_iterator end() const noexcept {
    return const_iterator(ctrl_, slots_, cap_, cap_, 0);
  }

  void clear() noexcept {
    if (cap_ == 0) {
      return;
    }
    destroyAll();
    std::memset(ctrl_, detail::kSwissEmpty,
                cap_ + detail::SwissGroup::kWidth);
    size_ = 0;
    growth_left_ = detail::swissCapacityToGrowth(cap_);
  }

  void reserve(size_t count) {
    if (count == 0) {
      return;
    }
    size_t target_cap = detail::swissSizeToCapacity(count);
    if (target_cap > cap_) {
      rehash(target_cap);
    }
  }

  uint64_t hashKey(const K &key) const noexcept {
    return fastMix64(static_cast<uint64_t>(hasher_(key)));
  }

  iterator find(const K &key) noexcept {
    return findWithHash(key, hashKey(key));
  }

  const_iterator find(const K &key) const noexcept {
    return findWithHash(key, hashKey(key));
  }

  iterator findWithHash(const K &key, uint64_t h) noexcept {
    size_t idx = findIndex(key, h);
    if (idx == cap_) {
      return end();
    }
    return iterator(ctrl_, slots_, idx, cap_, 0);
  }

  const_iterator findWithHash(const K &key, uint64_t h) const noexcept {
    size_t idx = findIndex(key, h);
    if (idx == cap_) {
      return end();
    }
    return const_iterator(ctrl_, slots_, idx, cap_, 0);
  }

  size_t count(const K &key) const noexcept {
    return find(key) != end() ? 1 : 0;
  }

  template <typename KeyArg, typename... Args>
  std::pair<iterator, bool> try_emplace(KeyArg &&key, Args &&...args) {
    return tryEmplaceWithHash(hashKey(key), std::forward<KeyArg>(key),
                              std::forward<Args>(args)...);
  }

  template <typename KeyArg, typename... Args>
  std::pair<iterator, bool> tryEmplaceWithHash(uint64_t h, KeyArg &&key,
                                               Args &&...args) {
    if (growth_left_ == 0) {
      growForInsert();
    }
    size_t mask = cap_ - 1;
    int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    size_t first_deleted = cap_;

    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        if (equal_(slots_[slot].kv.first, key)) {
          return {iterator(ctrl_, slots_, slot, cap_, 0), false};
        }
      }
      if (first_deleted == cap_) {
        auto avail = g.matchEmptyOrDeleted();
        if (avail) {
          unsigned lane = detail::SwissGroup::lowestBit(avail);
          first_deleted = (group_idx + lane) & mask;
        }
      }
      if (auto empty_mask = g.matchEmpty()) {
        size_t insert_slot = first_deleted != cap_
                                 ? first_deleted
                                 : ((group_idx +
                                     detail::SwissGroup::lowestBit(empty_mask)) &
                                    mask);
        bool was_empty = detail::swissIsEmpty(ctrl_[insert_slot]);
        setCtrl(insert_slot, h2);
        ::new (&slots_[insert_slot].kv)
            value_type(std::piecewise_construct,
                       std::forward_as_tuple(std::forward<KeyArg>(key)),
                       std::forward_as_tuple(std::forward<Args>(args)...));
        ++size_;
        if (was_empty) {
          --growth_left_;
        }
        return {iterator(ctrl_, slots_, insert_slot, cap_, 0), true};
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  template <typename KeyArg, typename ValArg>
  std::pair<iterator, bool> emplace(KeyArg &&key, ValArg &&val) {
    return try_emplace(std::forward<KeyArg>(key), std::forward<ValArg>(val));
  }

  V &operator[](const K &key) { return try_emplace(key).first->second; }
  V &operator[](K &&key) { return try_emplace(std::move(key)).first->second; }

  void erase(iterator it) noexcept {
    if (it == end()) {
      return;
    }
    eraseSlot(it.idx_);
  }

  size_t erase(const K &key) noexcept {
    size_t idx = findIndex(key, hashKey(key));
    if (idx == cap_) {
      return 0;
    }
    eraseSlot(idx);
    return 1;
  }

  friend bool operator==(const FlatHashMap &lhs, const FlatHashMap &rhs) {
    if (lhs.size_ != rhs.size_) {
      return false;
    }
    for (const auto &[k, v] : lhs) {
      auto it = rhs.find(k);
      if (it == rhs.end() || !(it->second == v)) {
        return false;
      }
    }
    return true;
  }

  friend bool operator!=(const FlatHashMap &lhs, const FlatHashMap &rhs) {
    return !(lhs == rhs);
  }

private:
  void allocate(size_t new_cap) {
    cap_ = new_cap;
    ctrl_ = static_cast<int8_t *>(
        ::operator new(cap_ + detail::SwissGroup::kWidth));
    std::memset(ctrl_, detail::kSwissEmpty,
                cap_ + detail::SwissGroup::kWidth);
    slots_ = static_cast<Slot *>(
        ::operator new(sizeof(Slot) * cap_, std::align_val_t{alignof(Slot)}));
    growth_left_ = detail::swissCapacityToGrowth(cap_);
  }

  void destroyAll() noexcept {
    if (!ctrl_) {
      return;
    }
    if constexpr (!std::is_trivially_destructible_v<K> ||
                  !std::is_trivially_destructible_v<V>) {
      for (size_t i = 0; i < cap_; ++i) {
        if (detail::swissIsFull(ctrl_[i])) {
          slots_[i].kv.~value_type();
        }
      }
    }
  }

  void deallocate() noexcept {
    if (ctrl_) {
      ::operator delete(ctrl_);
      ctrl_ = nullptr;
    }
    if (slots_) {
      ::operator delete(slots_, std::align_val_t{alignof(Slot)});
      slots_ = nullptr;
    }
    cap_ = 0;
    size_ = 0;
    growth_left_ = 0;
  }

  inline void setCtrl(size_t idx, int8_t val) noexcept {
    ctrl_[idx] = val;
    if (idx < detail::SwissGroup::kWidth) {
      ctrl_[idx + cap_] = val;
    }
  }

  size_t findIndex(const K &key, uint64_t h) const noexcept {
    if (size_ == 0) {
      return cap_;
    }
    size_t mask = cap_ - 1;
    int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        if (equal_(slots_[slot].kv.first, key)) {
          return slot;
        }
      }
      if (g.matchEmpty()) {
        return cap_;
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  void eraseSlot(size_t idx) noexcept {
    slots_[idx].kv.~value_type();
    --size_;
    size_t mask = cap_ - 1;
    size_t prev_group = (idx - detail::SwissGroup::kWidth) & mask;
    // Check if we can safely mark Empty instead of Deleted (no probe chain
    // crosses this slot if both the group ending before idx and starting at idx
    // have an Empty slot — conservative simple rule: check group at idx and
    // previous window).
    detail::SwissGroup before(ctrl_ + prev_group);
    detail::SwissGroup after(ctrl_ + idx);
    auto m_before = before.matchEmpty();
    auto m_after = after.matchEmpty();
    if (m_before && m_after &&
        detail::SwissGroup::lowestBit(m_after) <=
            detail::SwissGroup::highestBit(m_before)) {
      setCtrl(idx, detail::kSwissEmpty);
      ++growth_left_;
    } else {
      setCtrl(idx, detail::kSwissDeleted);
    }
  }

  void growForInsert() {
    if (cap_ == 0) {
      allocate(detail::SwissGroup::kWidth);
      return;
    }
    // If tombstones exhausted growth_left_, rehash in-place at same capacity
    // when load is still under half; otherwise double capacity.
    if (size_ * 2 < cap_) {
      rehash(cap_);
    } else {
      rehash(cap_ * 2);
    }
  }

  void rehash(size_t new_cap) {
    int8_t *old_ctrl = ctrl_;
    Slot *old_slots = slots_;
    size_t old_cap = cap_;

    allocate(new_cap);
    size_t mask = cap_ - 1;
    size_t moved = 0;
    for (size_t i = 0; i < old_cap; ++i) {
      if (!detail::swissIsFull(old_ctrl[i])) {
        continue;
      }
      uint64_t h = hashKey(old_slots[i].mutable_kv.first);
      int8_t h2 = detail::swissH2(h);
      size_t group_idx = static_cast<size_t>(h) & mask;
      size_t probe_step = 0;
      for (;;) {
        detail::SwissGroup g(ctrl_ + group_idx);
        if (auto empty_mask = g.matchEmpty()) {
          size_t slot =
              (group_idx + detail::SwissGroup::lowestBit(empty_mask)) & mask;
          setCtrl(slot, h2);
          ::new (&slots_[slot].mutable_kv)
              MutablePair(std::move(old_slots[i].mutable_kv));
          old_slots[i].kv.~value_type();
          ++moved;
          break;
        }
        probe_step += detail::SwissGroup::kWidth;
        group_idx = (group_idx + probe_step) & mask;
      }
    }
    size_ = moved;
    growth_left_ = detail::swissCapacityToGrowth(cap_) - size_;
    if (old_ctrl) {
      ::operator delete(old_ctrl);
    }
    if (old_slots) {
      ::operator delete(old_slots, std::align_val_t{alignof(Slot)});
    }
  }

  int8_t *ctrl_ = nullptr;
  Slot *slots_ = nullptr;
  size_t cap_ = 0;
  size_t size_ = 0;
  size_t growth_left_ = 0;
  Hash hasher_{};
  KeyEqual equal_{};
};

// Compact SwissTable index storing only 1-byte ctrl + 4-byte uint32_t handle
// per slot. External storage holds the keys and cached 64-bit hashes, so
// negative lookups touch only the ctrl array and rehashes never re-hash keys.
class CompactHashIndex {
public:
  static constexpr uint32_t kInvalid = UINT32_MAX;

  CompactHashIndex() = default;

  CompactHashIndex(const CompactHashIndex &other) {
    if (other.cap_ == 0) {
      return;
    }
    allocate(other.cap_);
    std::memcpy(ctrl_, other.ctrl_, cap_ + detail::SwissGroup::kWidth);
    std::memcpy(slots_, other.slots_, sizeof(uint32_t) * cap_);
    size_ = other.size_;
    growth_left_ = other.growth_left_;
  }

  CompactHashIndex(CompactHashIndex &&other) noexcept
      : ctrl_(other.ctrl_), slots_(other.slots_), cap_(other.cap_),
        size_(other.size_), growth_left_(other.growth_left_) {
    other.ctrl_ = nullptr;
    other.slots_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.growth_left_ = 0;
  }

  CompactHashIndex &operator=(const CompactHashIndex &other) {
    if (this == &other) {
      return *this;
    }
    CompactHashIndex tmp(other);
    swap(tmp);
    return *this;
  }

  CompactHashIndex &operator=(CompactHashIndex &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    deallocate();
    ctrl_ = other.ctrl_;
    slots_ = other.slots_;
    cap_ = other.cap_;
    size_ = other.size_;
    growth_left_ = other.growth_left_;
    other.ctrl_ = nullptr;
    other.slots_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.growth_left_ = 0;
    return *this;
  }

  ~CompactHashIndex() { deallocate(); }

  void swap(CompactHashIndex &other) noexcept {
    std::swap(ctrl_, other.ctrl_);
    std::swap(slots_, other.slots_);
    std::swap(cap_, other.cap_);
    std::swap(size_, other.size_);
    std::swap(growth_left_, other.growth_left_);
  }

  bool empty() const noexcept { return size_ == 0; }
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return cap_; }

  void clear() noexcept {
    if (cap_ == 0) {
      return;
    }
    std::memset(ctrl_, detail::kSwissEmpty,
                cap_ + detail::SwissGroup::kWidth);
    size_ = 0;
    growth_left_ = detail::swissCapacityToGrowth(cap_);
  }

  template <typename HashAt>
  void reserve(size_t count, HashAt &&hash_at) {
    if (count == 0) {
      return;
    }
    size_t target = detail::swissSizeToCapacity(count);
    if (target > cap_) {
      rehash(target, std::forward<HashAt>(hash_at));
    }
  }

  template <typename Eq>
  uint32_t find(uint64_t h, Eq &&eq) const noexcept {
    if (size_ == 0) {
      return kInvalid;
    }
    size_t mask = cap_ - 1;
    int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        uint32_t handle = slots_[slot];
        if (eq(handle)) {
          return handle;
        }
      }
      if (g.matchEmpty()) {
        return kInvalid;
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  // Finds existing handle matching `eq`, or inserts `new_handle` (obtained via
  // `make_handle()`) in a single probe pass. Returns {handle, inserted}.
  template <typename Eq, typename MakeHandle, typename HashAt>
  std::pair<uint32_t, bool> findOrInsert(uint64_t h, Eq &&eq,
                                         MakeHandle &&make_handle,
                                         HashAt &&hash_at) {
    if (growth_left_ == 0) {
      growForInsert(std::forward<HashAt>(hash_at));
    }
    size_t mask = cap_ - 1;
    int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    size_t first_deleted = cap_;

    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        uint32_t handle = slots_[slot];
        if (eq(handle)) {
          return {handle, false};
        }
      }
      if (first_deleted == cap_) {
        auto avail = g.matchEmptyOrDeleted();
        if (avail) {
          unsigned lane = detail::SwissGroup::lowestBit(avail);
          first_deleted = (group_idx + lane) & mask;
        }
      }
      if (auto empty_mask = g.matchEmpty()) {
        size_t insert_slot = first_deleted != cap_
                                 ? first_deleted
                                 : ((group_idx +
                                     detail::SwissGroup::lowestBit(empty_mask)) &
                                    mask);
        bool was_empty = detail::swissIsEmpty(ctrl_[insert_slot]);
        uint32_t handle = make_handle();
        setCtrl(insert_slot, h2);
        slots_[insert_slot] = handle;
        ++size_;
        if (was_empty) {
          --growth_left_;
        }
        return {handle, true};
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  template <typename Eq>
  bool erase(uint64_t h, Eq &&eq) noexcept {
    if (size_ == 0) {
      return false;
    }
    size_t mask = cap_ - 1;
    int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        if (eq(slots_[slot])) {
          eraseSlot(slot);
          return true;
        }
      }
      if (g.matchEmpty()) {
        return false;
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  template <typename Fn> void forEach(Fn &&fn) const {
    for (size_t i = 0; i < cap_; ++i) {
      if (detail::swissIsFull(ctrl_[i])) {
        fn(slots_[i]);
      }
    }
  }

private:
  void allocate(size_t new_cap) {
    cap_ = new_cap;
    ctrl_ = static_cast<int8_t *>(
        ::operator new(cap_ + detail::SwissGroup::kWidth));
    std::memset(ctrl_, detail::kSwissEmpty,
                cap_ + detail::SwissGroup::kWidth);
    slots_ = static_cast<uint32_t *>(::operator new(sizeof(uint32_t) * cap_));
    growth_left_ = detail::swissCapacityToGrowth(cap_);
  }

  void deallocate() noexcept {
    if (ctrl_) {
      ::operator delete(ctrl_);
      ctrl_ = nullptr;
    }
    if (slots_) {
      ::operator delete(slots_);
      slots_ = nullptr;
    }
    cap_ = 0;
    size_ = 0;
    growth_left_ = 0;
  }

  inline void setCtrl(size_t idx, int8_t val) noexcept {
    ctrl_[idx] = val;
    if (idx < detail::SwissGroup::kWidth) {
      ctrl_[idx + cap_] = val;
    }
  }

  void eraseSlot(size_t idx) noexcept {
    --size_;
    size_t mask = cap_ - 1;
    size_t prev_group = (idx - detail::SwissGroup::kWidth) & mask;
    detail::SwissGroup before(ctrl_ + prev_group);
    detail::SwissGroup after(ctrl_ + idx);
    auto m_before = before.matchEmpty();
    auto m_after = after.matchEmpty();
    if (m_before && m_after &&
        detail::SwissGroup::lowestBit(m_after) <=
            detail::SwissGroup::highestBit(m_before)) {
      setCtrl(idx, detail::kSwissEmpty);
      ++growth_left_;
    } else {
      setCtrl(idx, detail::kSwissDeleted);
    }
  }

  template <typename HashAt> void growForInsert(HashAt &&hash_at) {
    if (cap_ == 0) {
      allocate(detail::SwissGroup::kWidth);
      return;
    }
    if (size_ * 2 < cap_) {
      rehash(cap_, std::forward<HashAt>(hash_at));
    } else {
      rehash(cap_ * 2, std::forward<HashAt>(hash_at));
    }
  }

  template <typename HashAt>
  void rehash(size_t new_cap, HashAt &&hash_at) {
    int8_t *old_ctrl = ctrl_;
    uint32_t *old_slots = slots_;
    size_t old_cap = cap_;

    allocate(new_cap);
    size_t mask = cap_ - 1;
    size_t moved = 0;
    for (size_t i = 0; i < old_cap; ++i) {
      if (!detail::swissIsFull(old_ctrl[i])) {
        continue;
      }
      uint32_t handle = old_slots[i];
      uint64_t h = hash_at(handle);
      int8_t h2 = detail::swissH2(h);
      size_t group_idx = static_cast<size_t>(h) & mask;
      size_t probe_step = 0;
      for (;;) {
        detail::SwissGroup g(ctrl_ + group_idx);
        if (auto empty_mask = g.matchEmpty()) {
          size_t slot =
              (group_idx + detail::SwissGroup::lowestBit(empty_mask)) & mask;
          setCtrl(slot, h2);
          slots_[slot] = handle;
          ++moved;
          break;
        }
        probe_step += detail::SwissGroup::kWidth;
        group_idx = (group_idx + probe_step) & mask;
      }
    }
    size_ = moved;
    growth_left_ = detail::swissCapacityToGrowth(cap_) - size_;
    if (old_ctrl) {
      ::operator delete(old_ctrl);
    }
    if (old_slots) {
      ::operator delete(old_slots);
    }
  }

  int8_t *ctrl_ = nullptr;
  uint32_t *slots_ = nullptr;
  size_t cap_ = 0;
  size_t size_ = 0;
  size_t growth_left_ = 0;
};

// Lightweight unchecked-append buffer for trivially copyable POD structs.
// Eliminates per-insert capacity branches and zero-initialization overhead in
// hoisted batch hot loops while matching std::vector's interface.
template <typename T> class PodVec {
  static_assert(std::is_trivially_copyable_v<T>,
                "PodVec requires trivially copyable element types");

public:
  PodVec() = default;

  PodVec(const PodVec &other) {
    if (other.size_ > 0) {
      allocate(other.size_);
      std::memcpy(data_, other.data_, sizeof(T) * other.size_);
      size_ = other.size_;
    }
  }

  PodVec(PodVec &&other) noexcept
      : data_(other.data_), size_(other.size_), cap_(other.cap_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.cap_ = 0;
  }

  PodVec &operator=(const PodVec &other) {
    if (this == &other) {
      return *this;
    }
    PodVec tmp(other);
    swap(tmp);
    return *this;
  }

  PodVec &operator=(PodVec &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    deallocate();
    data_ = other.data_;
    size_ = other.size_;
    cap_ = other.cap_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.cap_ = 0;
    return *this;
  }

  ~PodVec() { deallocate(); }

  void swap(PodVec &other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(cap_, other.cap_);
  }

  bool empty() const noexcept { return size_ == 0; }
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return cap_; }
  T *data() noexcept { return data_; }
  const T *data() const noexcept { return data_; }
  T *begin() noexcept { return data_; }
  T *end() noexcept { return data_ + size_; }
  const T *begin() const noexcept { return data_; }
  const T *end() const noexcept { return data_ + size_; }
  T &operator[](size_t i) noexcept { return data_[i]; }
  const T &operator[](size_t i) const noexcept { return data_[i]; }
  T &back() noexcept { return data_[size_ - 1]; }
  const T &back() const noexcept { return data_[size_ - 1]; }
  void clear() noexcept { size_ = 0; }

  void reserve(size_t new_cap) {
    if (new_cap > cap_) {
      reallocate(new_cap);
    }
  }

  inline void reserveAdditional(size_t extra) {
    if (size_ + extra > cap_) {
      size_t target = std::max(cap_ ? cap_ * 2 : size_t(16), size_ + extra);
      reallocate(target);
    }
  }

  void resize(size_t n) {
    if (n > cap_) {
      reallocate(std::max(cap_ * 2, n));
    }
    if (n > size_) {
      for (size_t i = size_; i < n; ++i) {
        data_[i] = T{};
      }
    }
    size_ = n;
  }

  void resize(size_t n, const T &fill) {
    if (n > cap_) {
      reallocate(std::max(cap_ * 2, n));
    }
    if (n > size_) {
      for (size_t i = size_; i < n; ++i) {
        data_[i] = fill;
      }
    }
    size_ = n;
  }

  inline void push_back(const T &val) {
    if (size_ == cap_) {
      reallocate(cap_ ? cap_ * 2 : size_t(16));
    }
    data_[size_++] = val;
  }

  template <typename... Args> inline void emplace_back(Args &&...args) {
    if (size_ == cap_) {
      reallocate(cap_ ? cap_ * 2 : size_t(16));
    }
    ::new (data_ + size_) T(std::forward<Args>(args)...);
    ++size_;
  }

  inline void pushBackUnchecked(const T &val) noexcept {
    data_[size_++] = val;
  }

  inline void setSizeUnchecked(size_t new_size) noexcept { size_ = new_size; }

private:
  void allocate(size_t n) {
    data_ = static_cast<T *>(
        ::operator new(sizeof(T) * n, std::align_val_t{alignof(T)}));
    cap_ = n;
  }

  void reallocate(size_t new_cap) {
    T *new_data = static_cast<T *>(
        ::operator new(sizeof(T) * new_cap, std::align_val_t{alignof(T)}));
    if (size_ > 0) {
      std::memcpy(new_data, data_, sizeof(T) * size_);
    }
    if (data_) {
      ::operator delete(data_, std::align_val_t{alignof(T)});
    }
    data_ = new_data;
    cap_ = new_cap;
  }

  void deallocate() noexcept {
    if (data_) {
      ::operator delete(data_, std::align_val_t{alignof(T)});
      data_ = nullptr;
    }
    size_ = 0;
    cap_ = 0;
  }

  T *data_ = nullptr;
  size_t size_ = 0;
  size_t cap_ = 0;
};

// Specialized 16-byte-per-slot NEON/SSE2 SwissTable for exact binary e-nodes
// keyed by packed (c0, c1) -> (nidx, owner).
// 1-byte L2-resident ctrl array avoids reading slot memory on misses and keeps
// probe length ~1.0 even at 87.5% (7/8) load factor; write-intent prefetch and
// 16-ahead prefetched rehash hide DRAM latency!
class BinaryMemoTable {
public:
  struct Slot {
    uint64_t key;
    uint32_t nidx;
    Id owner;
  };

  static inline uint64_t packKey(Id c0, Id c1) noexcept {
    return (static_cast<uint64_t>(c0.value()) << 32) |
           static_cast<uint64_t>(c1.value());
  }

  BinaryMemoTable() = default;

  BinaryMemoTable(const BinaryMemoTable &other) {
    if (other.cap_ == 0) {
      return;
    }
    allocate(other.cap_);
    std::memcpy(ctrl_, other.ctrl_, cap_ + detail::SwissGroup::kWidth);
    std::memcpy(slots_, other.slots_, sizeof(Slot) * cap_);
    size_ = other.size_;
    growth_left_ = other.growth_left_;
  }

  BinaryMemoTable(BinaryMemoTable &&other) noexcept
      : ctrl_(other.ctrl_), slots_(other.slots_), cap_(other.cap_),
        size_(other.size_), growth_left_(other.growth_left_) {
    other.ctrl_ = nullptr;
    other.slots_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.growth_left_ = 0;
  }

  BinaryMemoTable &operator=(const BinaryMemoTable &other) {
    if (this == &other) {
      return *this;
    }
    BinaryMemoTable tmp(other);
    swap(tmp);
    return *this;
  }

  BinaryMemoTable &operator=(BinaryMemoTable &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    deallocate();
    ctrl_ = other.ctrl_;
    slots_ = other.slots_;
    cap_ = other.cap_;
    size_ = other.size_;
    growth_left_ = other.growth_left_;
    other.ctrl_ = nullptr;
    other.slots_ = nullptr;
    other.cap_ = 0;
    other.size_ = 0;
    other.growth_left_ = 0;
    return *this;
  }

  ~BinaryMemoTable() { deallocate(); }

  void swap(BinaryMemoTable &other) noexcept {
    std::swap(ctrl_, other.ctrl_);
    std::swap(slots_, other.slots_);
    std::swap(cap_, other.cap_);
    std::swap(size_, other.size_);
    std::swap(growth_left_, other.growth_left_);
  }

  bool empty() const noexcept { return size_ == 0; }
  size_t size() const noexcept { return size_; }

  void reserve(size_t count) {
    if (count == 0) {
      return;
    }
    size_t target = detail::swissSizeToCapacity(count);
    if (target > cap_) {
      rehash(target);
    }
  }

  inline void ensureSpace(size_t extra) {
    if (growth_left_ < extra) {
      growForAdditional(extra);
    }
  }

  inline void prefetch(uint64_t h) const noexcept {
    if (cap_ == 0) {
      return;
    }
    const size_t idx = static_cast<size_t>(h) & (cap_ - 1);
    __builtin_prefetch(ctrl_ + idx, 1, 1);
    __builtin_prefetch(slots_ + idx, 1, 1);
  }

  Slot *find(uint64_t key, uint64_t h) noexcept {
    if (size_ == 0) {
      return nullptr;
    }
    const size_t mask = cap_ - 1;
    const int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        if (slots_[slot].key == key) {
          return &slots_[slot];
        }
      }
      if (g.matchEmpty()) {
        return nullptr;
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  const Slot *find(uint64_t key, uint64_t h) const noexcept {
    return const_cast<BinaryMemoTable *>(this)->find(key, h);
  }

  template <typename MakeEntry>
  inline std::pair<Slot *, bool> findOrInsertUnchecked(uint64_t key, uint64_t h,
                                                       MakeEntry &&make_entry) {
    const size_t mask = cap_ - 1;
    const int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    size_t first_deleted = cap_;

    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        if (slots_[slot].key == key) {
          return {&slots_[slot], false};
        }
      }
      if (first_deleted == cap_) {
        auto avail = g.matchEmptyOrDeleted();
        if (avail) {
          unsigned lane = detail::SwissGroup::lowestBit(avail);
          first_deleted = (group_idx + lane) & mask;
        }
      }
      if (auto empty_mask = g.matchEmpty()) {
        size_t insert_slot = first_deleted != cap_
                                 ? first_deleted
                                 : ((group_idx +
                                     detail::SwissGroup::lowestBit(empty_mask)) &
                                    mask);
        bool was_empty = detail::swissIsEmpty(ctrl_[insert_slot]);
        auto [nidx, owner] = make_entry();
        setCtrl(insert_slot, h2);
        slots_[insert_slot] = Slot{key, nidx, owner};
        ++size_;
        if (was_empty) {
          --growth_left_;
        }
        return {&slots_[insert_slot], true};
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  template <typename MakeEntry>
  std::pair<Slot *, bool> findOrInsert(uint64_t key, uint64_t h,
                                       MakeEntry &&make_entry) {
    if (growth_left_ == 0) {
      growForAdditional(1);
    }
    return findOrInsertUnchecked(key, h, std::forward<MakeEntry>(make_entry));
  }

  bool erase(uint64_t key, uint64_t h, uint32_t expected_nidx) noexcept {
    if (size_ == 0) {
      return false;
    }
    const size_t mask = cap_ - 1;
    const int8_t h2 = detail::swissH2(h);
    size_t group_idx = static_cast<size_t>(h) & mask;
    size_t probe_step = 0;
    for (;;) {
      detail::SwissGroup g(ctrl_ + group_idx);
      auto matches = g.match(h2);
      while (matches) {
        unsigned lane = detail::SwissGroup::lowestBit(matches);
        matches = detail::SwissGroup::clearLowestBit(matches, lane);
        size_t slot = (group_idx + lane) & mask;
        if (slots_[slot].key == key && slots_[slot].nidx == expected_nidx) {
          eraseSlot(slot);
          return true;
        }
      }
      if (g.matchEmpty()) {
        return false;
      }
      probe_step += detail::SwissGroup::kWidth;
      group_idx = (group_idx + probe_step) & mask;
    }
  }

  template <typename Fn> void forEach(Fn &&fn) const {
    for (size_t i = 0; i < cap_; ++i) {
      if (detail::swissIsFull(ctrl_[i])) {
        fn(slots_[i]);
      }
    }
  }

private:
  void allocate(size_t new_cap) {
    cap_ = new_cap;
    ctrl_ = static_cast<int8_t *>(
        ::operator new(cap_ + detail::SwissGroup::kWidth));
    std::memset(ctrl_, detail::kSwissEmpty,
                cap_ + detail::SwissGroup::kWidth);
    slots_ = static_cast<Slot *>(
        ::operator new(sizeof(Slot) * cap_, std::align_val_t{alignof(Slot)}));
    growth_left_ = detail::swissCapacityToGrowth(cap_);
  }

  void deallocate() noexcept {
    if (ctrl_) {
      ::operator delete(ctrl_);
      ctrl_ = nullptr;
    }
    if (slots_) {
      ::operator delete(slots_, std::align_val_t{alignof(Slot)});
      slots_ = nullptr;
    }
    cap_ = 0;
    size_ = 0;
    growth_left_ = 0;
  }

  inline void setCtrl(size_t idx, int8_t val) noexcept {
    ctrl_[idx] = val;
    if (idx < detail::SwissGroup::kWidth) {
      ctrl_[idx + cap_] = val;
    }
  }

  void eraseSlot(size_t idx) noexcept {
    --size_;
    const size_t mask = cap_ - 1;
    const size_t prev_group = (idx - detail::SwissGroup::kWidth) & mask;
    detail::SwissGroup before(ctrl_ + prev_group);
    detail::SwissGroup after(ctrl_ + idx);
    auto m_before = before.matchEmpty();
    auto m_after = after.matchEmpty();
    if (m_before && m_after &&
        detail::SwissGroup::lowestBit(m_after) <=
            detail::SwissGroup::highestBit(m_before)) {
      setCtrl(idx, detail::kSwissEmpty);
      ++growth_left_;
    } else {
      setCtrl(idx, detail::kSwissDeleted);
    }
  }

  void growForAdditional(size_t extra) {
    if (cap_ == 0) {
      allocate(std::max<size_t>(detail::SwissGroup::kWidth,
                                detail::swissSizeToCapacity(extra)));
      return;
    }
    if ((size_ + extra) * 2 <= cap_) {
      rehash(cap_);
    } else {
      size_t target =
          std::max(cap_ * 2, detail::swissSizeToCapacity(size_ + extra));
      rehash(target);
    }
  }

  void rehash(size_t new_cap) {
    int8_t *old_ctrl = ctrl_;
    Slot *old_slots = slots_;
    size_t old_cap = cap_;

    allocate(new_cap);
    const size_t mask = cap_ - 1;

    constexpr size_t kPf = 16;
    Slot q_slot[kPf];
    size_t q_gidx[kPf];
    int8_t q_h2[kPf];
    size_t q_head = 0;
    size_t q_count = 0;
    size_t moved = 0;

    auto flushOne = [&]() noexcept {
      const Slot s = q_slot[q_head];
      size_t group_idx = q_gidx[q_head];
      const int8_t h2 = q_h2[q_head];
      q_head = (q_head + 1) & (kPf - 1);
      --q_count;
      size_t probe_step = 0;
      for (;;) {
        detail::SwissGroup g(ctrl_ + group_idx);
        if (auto empty_mask = g.matchEmpty()) {
          size_t slot =
              (group_idx + detail::SwissGroup::lowestBit(empty_mask)) & mask;
          setCtrl(slot, h2);
          slots_[slot] = s;
          ++moved;
          return;
        }
        probe_step += detail::SwissGroup::kWidth;
        group_idx = (group_idx + probe_step) & mask;
      }
    };

    for (size_t i = 0; i < old_cap; ++i) {
      if (!detail::swissIsFull(old_ctrl[i])) {
        continue;
      }
      const Slot s = old_slots[i];
      const uint64_t h = fastMix64(s.key);
      const size_t gidx = static_cast<size_t>(h) & mask;
      __builtin_prefetch(ctrl_ + gidx, 1, 1);
      __builtin_prefetch(slots_ + gidx, 1, 1);
      const size_t tail = (q_head + q_count) & (kPf - 1);
      q_slot[tail] = s;
      q_gidx[tail] = gidx;
      q_h2[tail] = detail::swissH2(h);
      ++q_count;
      if (q_count == kPf) {
        flushOne();
      }
    }
    while (q_count > 0) {
      flushOne();
    }

    size_ = moved;
    growth_left_ = detail::swissCapacityToGrowth(cap_) - size_;
    if (old_ctrl) {
      ::operator delete(old_ctrl);
    }
    if (old_slots) {
      ::operator delete(old_slots, std::align_val_t{alignof(Slot)});
    }
  }

  int8_t *ctrl_ = nullptr;
  Slot *slots_ = nullptr;
  size_t cap_ = 0;
  size_t size_ = 0;
  size_t growth_left_ = 0;
};

} // namespace lotus::egraph
