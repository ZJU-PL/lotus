//===- SVFGNodeBS.h -- Compact sparse points-to set -------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// SVFGNodeBS: points-to set of abstract object IDs.
//
// IDs are stored in a sparse bitset of sorted 256-bit chunks (4 x 64-bit
// words), each covering 256 consecutive IDs:
// - 0 or 1 chunk: stored inline inside the 48-byte object (zero heap mallocs).
// - >= 2 chunks: stored in a compact heap array of AoS `Word` chunks so
//   `base` and `bits[lane]` share the same 64-byte cache line on random
//   `insertBit` / `count` probes, with galloping (exponential) search and
//   O(1) tail-append / range fast-reject paths on set operations.
//
// The API is the subset of std::set used by Lotus (insert/count/find/erase/
// iteration/==/<), including the `insert(iterator, value)` form required by
// std::inserter. Iteration is in increasing order, so std::set_intersection
// and similar algorithms behave as before.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <set>
#include <vector>

namespace lotus {
namespace analysis {

class SVFGNodeBS {
public:
  using value_type = std::uint32_t;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = std::uint32_t;
  using const_reference = std::uint32_t;

private:
  /// A dense 256-bit chunk covering IDs [base * 256, base * 256 + 255].
  /// Stored sorted by `base` and never containing an all-zero chunk.
  struct Word {
    std::uint32_t base = 0;
    std::uint64_t bits[4] = {0, 0, 0, 0};

    bool any() const noexcept {
      return (bits[0] | bits[1] | bits[2] | bits[3]) != 0;
    }
    size_type popcount() const noexcept {
      return static_cast<size_type>(__builtin_popcountll(bits[0]) +
                                    __builtin_popcountll(bits[1]) +
                                    __builtin_popcountll(bits[2]) +
                                    __builtin_popcountll(bits[3]));
    }
    friend bool operator==(const Word &lhs, const Word &rhs) noexcept {
      return lhs.base == rhs.base && lhs.bits[0] == rhs.bits[0] &&
             lhs.bits[1] == rhs.bits[1] && lhs.bits[2] == rhs.bits[2] &&
             lhs.bits[3] == rhs.bits[3];
    }
    friend bool operator<(const Word &lhs, const Word &rhs) noexcept {
      return lhs.base < rhs.base;
    }
  };

  // Compact 48-byte inline-1-chunk SBO layout:
  // - `cap_ == 0`: inline mode (`numChunks_` is 0 or 1; chunk lives in
  //   `st_.inl`, zero heap allocations).
  // - `cap_ > 0`: heap mode (`st_.heap` points to `Word[cap_]`).
  std::uint32_t size_ = 0;
  std::uint32_t numChunks_ = 0;
  union Storage {
    Word inl;
    struct {
      Word *data;
      std::uint32_t cap;
    } heap;
    Storage() noexcept : inl{} {}
  } st_;
  bool isHeap_ = false;

  static Word *allocHeap(std::uint32_t cap) {
    return static_cast<Word *>(
        ::operator new(static_cast<std::size_t>(cap) * sizeof(Word)));
  }

  static void deallocHeap(Word *ptr) noexcept { ::operator delete(ptr); }

  Word *dataMut() noexcept { return isHeap_ ? st_.heap.data : &st_.inl; }
  const Word *data() const noexcept {
    return isHeap_ ? st_.heap.data : &st_.inl;
  }

  static constexpr std::uint32_t wordIndex(std::uint32_t id) noexcept {
    return id >> 8;
  }
  static constexpr unsigned subWord(std::uint32_t id) noexcept {
    return static_cast<unsigned>((id >> 6) & 3U);
  }
  static constexpr std::uint64_t bitMask(std::uint32_t id) noexcept {
    return std::uint64_t(1) << (id & 63U);
  }
  static Word makeSingleBitWord(std::uint32_t base, unsigned lane,
                                std::uint64_t mask) noexcept {
    Word w;
    w.base = base;
    w.bits[lane] = mask;
    return w;
  }

  static size_type lowerBound(const Word *arr, size_type n,
                              std::uint32_t target) noexcept {
    size_type lo = 0;
    size_type hi = n;
    while (lo < hi) {
      const size_type mid = lo + ((hi - lo) >> 1);
      if (arr[mid].base < target)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  static size_type gallopLowerBound(const Word *arr, size_type pos, size_type n,
                                    std::uint32_t target) noexcept {
    if (pos >= n || arr[pos].base >= target)
      return pos;
    size_type step = 1;
    size_type lo = pos;
    size_type hi = pos + 1;
    while (hi < n && arr[hi].base < target) {
      lo = hi;
      step <<= 1;
      hi = pos + step;
    }
    if (hi > n)
      hi = n;
    size_type l = lo + 1;
    size_type r = hi;
    while (l < r) {
      const size_type m = l + ((r - l) >> 1);
      if (arr[m].base < target)
        l = m + 1;
      else
        r = m;
    }
    return l;
  }

  void ensureHeapCapacity(std::uint32_t needed, bool preserve) {
    if (isHeap_ && st_.heap.cap >= needed)
      return;
    std::uint32_t newCap = isHeap_ ? (st_.heap.cap * 2) : 2U;
    if (newCap < needed)
      newCap = needed;
    Word *newBuf = allocHeap(newCap);
    if (preserve && numChunks_ > 0) {
      std::memcpy(newBuf, data(),
                  static_cast<std::size_t>(numChunks_) * sizeof(Word));
    }
    if (isHeap_)
      deallocHeap(st_.heap.data);
    st_.heap.data = newBuf;
    st_.heap.cap = newCap;
    isHeap_ = true;
  }

  void insertChunkAt(size_type index, const Word &w) {
    if (!isHeap_ && numChunks_ == 0) {
      st_.inl = w;
      numChunks_ = 1;
      return;
    }
    const std::uint32_t n = numChunks_;
    if (!isHeap_ || n >= st_.heap.cap) {
      const std::uint32_t newCap = isHeap_ ? (st_.heap.cap * 2) : 2U;
      Word *newBuf = allocHeap(newCap);
      const Word *old = data();
      if (index > 0)
        std::memcpy(newBuf, old, index * sizeof(Word));
      newBuf[index] = w;
      const size_type tail = n - index;
      if (tail > 0)
        std::memcpy(newBuf + index + 1, old + index, tail * sizeof(Word));
      if (isHeap_)
        deallocHeap(st_.heap.data);
      st_.heap.data = newBuf;
      st_.heap.cap = newCap;
      isHeap_ = true;
      numChunks_ = n + 1;
      return;
    }
    Word *dst = st_.heap.data;
    const size_type tail = n - index;
    if (tail > 0)
      std::memmove(dst + index + 1, dst + index, tail * sizeof(Word));
    dst[index] = w;
    numChunks_ = n + 1;
  }

  void appendChunk(const Word &w) {
    if (!isHeap_ && numChunks_ == 0) {
      st_.inl = w;
      numChunks_ = 1;
      return;
    }
    if (!isHeap_ || numChunks_ >= st_.heap.cap)
      ensureHeapCapacity(numChunks_ + 1, /*preserve=*/true);
    st_.heap.data[numChunks_++] = w;
  }

  static std::vector<Word> &mergeScratch() {
    static thread_local std::vector<Word> merged;
    return merged;
  }

public:
  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::uint32_t;
    using difference_type = std::ptrdiff_t;
    using pointer = const std::uint32_t *;
    using reference = std::uint32_t;

  private:
    const SVFGNodeBS *owner_ = nullptr;
    size_type chunk_ = 0;
    unsigned lane_ = 0;
    std::uint64_t pending_ = 0;
    std::uint32_t current_ = 0;

    void advance() noexcept {
      if (!owner_)
        return;
      const Word *arr = owner_->data();
      while (pending_ == 0) {
        if (++lane_ < 4U) {
          pending_ = arr[chunk_].bits[lane_];
        } else {
          ++chunk_;
          if (chunk_ >= owner_->numChunks_) {
            lane_ = 0;
            current_ = 0;
            return;
          }
          lane_ = 0;
          pending_ = arr[chunk_].bits[0];
        }
      }
      const unsigned bit = static_cast<unsigned>(__builtin_ctzll(pending_));
      current_ = (arr[chunk_].base << 8) | (lane_ << 6) | bit;
      pending_ &= pending_ - 1;
    }

  public:
    const_iterator() = default;

    const_iterator(const SVFGNodeBS *owner, size_type chunk, unsigned lane,
                   std::uint64_t pending, bool doAdvance = true) noexcept
        : owner_(owner), chunk_(chunk), lane_(lane), pending_(pending) {
      if (doAdvance)
        advance();
    }

    std::uint32_t operator*() const noexcept { return current_; }
    pointer operator->() const noexcept { return &current_; }

    const_iterator &operator++() noexcept {
      advance();
      return *this;
    }
    const_iterator operator++(int) noexcept {
      const_iterator tmp = *this;
      advance();
      return tmp;
    }

    friend bool operator==(const const_iterator &lhs,
                           const const_iterator &rhs) noexcept {
      return lhs.chunk_ == rhs.chunk_ && lhs.current_ == rhs.current_ &&
             lhs.lane_ == rhs.lane_ && lhs.owner_ == rhs.owner_;
    }
    friend bool operator!=(const const_iterator &lhs,
                           const const_iterator &rhs) noexcept {
      return !(lhs == rhs);
    }
  };

  using iterator = const_iterator;

  SVFGNodeBS() noexcept = default;

  ~SVFGNodeBS() noexcept {
    if (isHeap_)
      deallocHeap(st_.heap.data);
  }

  SVFGNodeBS(const SVFGNodeBS &other) {
    if (other.numChunks_ == 0)
      return;
    size_ = other.size_;
    if (other.numChunks_ == 1) {
      numChunks_ = 1;
      st_.inl = other.data()[0];
      return;
    }
    const std::uint32_t n = other.numChunks_;
    Word *buf = allocHeap(n);
    std::memcpy(buf, other.data(), static_cast<std::size_t>(n) * sizeof(Word));
    st_.heap.data = buf;
    st_.heap.cap = n;
    isHeap_ = true;
    numChunks_ = n;
  }

  SVFGNodeBS(SVFGNodeBS &&other) noexcept
      : size_(other.size_), numChunks_(other.numChunks_),
        isHeap_(other.isHeap_) {
    if (isHeap_)
      st_.heap = other.st_.heap;
    else
      st_.inl = other.st_.inl;
    other.size_ = 0;
    other.numChunks_ = 0;
    other.isHeap_ = false;
  }

  SVFGNodeBS &operator=(const SVFGNodeBS &other) {
    if (this == &other)
      return *this;
    if (other.numChunks_ == 0) {
      numChunks_ = 0;
      size_ = 0;
      return *this;
    }
    if (other.numChunks_ == 1) {
      if (isHeap_)
        st_.heap.data[0] = other.data()[0];
      else
        st_.inl = other.data()[0];
      numChunks_ = 1;
      size_ = other.size_;
      return *this;
    }
    const std::uint32_t n = other.numChunks_;
    ensureHeapCapacity(n, /*preserve=*/false);
    std::memcpy(st_.heap.data, other.data(),
                static_cast<std::size_t>(n) * sizeof(Word));
    numChunks_ = n;
    size_ = other.size_;
    return *this;
  }

  SVFGNodeBS &operator=(SVFGNodeBS &&other) noexcept {
    if (this == &other)
      return *this;
    if (isHeap_)
      deallocHeap(st_.heap.data);
    size_ = other.size_;
    numChunks_ = other.numChunks_;
    isHeap_ = other.isHeap_;
    if (isHeap_)
      st_.heap = other.st_.heap;
    else
      st_.inl = other.st_.inl;
    other.size_ = 0;
    other.numChunks_ = 0;
    other.isHeap_ = false;
    return *this;
  }

  SVFGNodeBS(std::initializer_list<std::uint32_t> values) {
    for (std::uint32_t value : values)
      insertBit(value);
  }

  /// Converting constructor for call sites that build a std::set<uint32_t>.
  SVFGNodeBS(const std::set<std::uint32_t> &values) {
    for (std::uint32_t value : values)
      insertBit(value);
  }

  //===------------------------------------------------------------------===//
  // Capacity
  //===------------------------------------------------------------------===//

  bool empty() const noexcept { return size_ == 0; }
  size_type size() const noexcept { return size_; }

  //===------------------------------------------------------------------===//
  // Iteration (ascending object ID order)
  //===------------------------------------------------------------------===//

  const_iterator begin() const noexcept {
    if (numChunks_ == 0)
      return end();
    return const_iterator(this, 0, 0, data()[0].bits[0]);
  }
  const_iterator end() const noexcept {
    return const_iterator(this, numChunks_, 0, 0, false);
  }

  //===------------------------------------------------------------------===//
  // Lookup
  //===------------------------------------------------------------------===//

  bool contains(std::uint32_t id) const noexcept { return count(id) != 0; }

  size_type count(std::uint32_t id) const noexcept {
    const std::uint32_t n = numChunks_;
    if (n == 0)
      return 0;
    const std::uint32_t w = wordIndex(id);
    const Word *arr = data();
    if (n == 1) {
      if (arr[0].base != w)
        return 0;
      return (arr[0].bits[subWord(id)] & bitMask(id)) != 0 ? 1 : 0;
    }
    if (w > arr[n - 1].base || w < arr[0].base)
      return 0;
    const size_type index = lowerBound(arr, n, w);
    if (index == n || arr[index].base != w)
      return 0;
    return (arr[index].bits[subWord(id)] & bitMask(id)) != 0 ? 1 : 0;
  }

  const_iterator find(std::uint32_t id) const noexcept {
    const std::uint32_t n = numChunks_;
    if (n == 0)
      return end();
    const std::uint32_t w = wordIndex(id);
    const Word *arr = data();
    const size_type index =
        (n == 1) ? (arr[0].base == w ? 0 : 1) : lowerBound(arr, n, w);
    if (index == n || arr[index].base != w)
      return end();
    const unsigned lane = subWord(id);
    const std::uint64_t laneBits = arr[index].bits[lane];
    if ((laneBits & bitMask(id)) == 0)
      return end();
    return const_iterator(this, index, lane,
                          laneBits & (~std::uint64_t(0) << (id & 63U)));
  }

  //===------------------------------------------------------------------===//
  // Modifiers
  //===------------------------------------------------------------------===//

  /// Fast single-bit insertion returning true iff `id` was not already present.
  bool insertBit(std::uint32_t id) {
    const std::uint32_t w = wordIndex(id);
    const unsigned lane = subWord(id);
    const std::uint64_t mask = bitMask(id);
    const std::uint32_t n = numChunks_;
    if (n == 0) {
      appendChunk(makeSingleBitWord(w, lane, mask));
      size_ = 1;
      return true;
    }
    Word *arr = dataMut();
    Word &back = arr[n - 1];
    if (back.base == w) {
      if ((back.bits[lane] & mask) != 0)
        return false;
      back.bits[lane] |= mask;
      ++size_;
      return true;
    }
    if (back.base < w) {
      appendChunk(makeSingleBitWord(w, lane, mask));
      ++size_;
      return true;
    }
    if (n == 1 || w < arr[0].base) {
      insertChunkAt(0, makeSingleBitWord(w, lane, mask));
      ++size_;
      return true;
    }
    const size_type index = lowerBound(arr, n, w);
    if (index < n && arr[index].base == w) {
      if ((arr[index].bits[lane] & mask) != 0)
        return false;
      arr[index].bits[lane] |= mask;
      ++size_;
      return true;
    }
    insertChunkAt(index, makeSingleBitWord(w, lane, mask));
    ++size_;
    return true;
  }

  std::pair<iterator, bool> insert(std::uint32_t id) {
    const bool added = insertBit(id);
    return {find(id), added};
  }

  /// Union sorted chunks in linear/galloping time.
  bool unite(const SVFGNodeBS &other) {
    if (other.size_ == 0 || this == &other)
      return false;
    if (size_ == 0) {
      *this = other;
      return true;
    }
    const std::uint32_t nRight = other.numChunks_;
    const Word *rArr = other.data();
    if (nRight == 1) {
      const Word &ow = rArr[0];
      const std::uint32_t nLeft = numChunks_;
      Word *lArr = dataMut();
      Word &back = lArr[nLeft - 1];
      if (back.base == ow.base) {
        const std::uint64_t a0 = ow.bits[0] & ~back.bits[0];
        const std::uint64_t a1 = ow.bits[1] & ~back.bits[1];
        const std::uint64_t a2 = ow.bits[2] & ~back.bits[2];
        const std::uint64_t a3 = ow.bits[3] & ~back.bits[3];
        if ((a0 | a1 | a2 | a3) == 0)
          return false;
        back.bits[0] |= a0;
        back.bits[1] |= a1;
        back.bits[2] |= a2;
        back.bits[3] |= a3;
        size_ += static_cast<std::uint32_t>(
            __builtin_popcountll(a0) + __builtin_popcountll(a1) +
            __builtin_popcountll(a2) + __builtin_popcountll(a3));
        return true;
      }
      if (back.base < ow.base) {
        appendChunk(ow);
        size_ += other.size_;
        return true;
      }
      const size_type index =
          (nLeft == 1) ? 0 : lowerBound(lArr, nLeft, ow.base);
      if (index < nLeft && lArr[index].base == ow.base) {
        Word &lw = lArr[index];
        const std::uint64_t a0 = ow.bits[0] & ~lw.bits[0];
        const std::uint64_t a1 = ow.bits[1] & ~lw.bits[1];
        const std::uint64_t a2 = ow.bits[2] & ~lw.bits[2];
        const std::uint64_t a3 = ow.bits[3] & ~lw.bits[3];
        if ((a0 | a1 | a2 | a3) == 0)
          return false;
        lw.bits[0] |= a0;
        lw.bits[1] |= a1;
        lw.bits[2] |= a2;
        lw.bits[3] |= a3;
        size_ += static_cast<std::uint32_t>(
            __builtin_popcountll(a0) + __builtin_popcountll(a1) +
            __builtin_popcountll(a2) + __builtin_popcountll(a3));
        return true;
      }
      insertChunkAt(index, ow);
      size_ += other.size_;
      return true;
    }

    const std::uint32_t nLeft = numChunks_;
    Word *lArr = dataMut();

    if (lArr[nLeft - 1].base < rArr[0].base) {
      ensureHeapCapacity(nLeft + nRight, /*preserve=*/true);
      std::memcpy(st_.heap.data + nLeft, rArr,
                  static_cast<std::size_t>(nRight) * sizeof(Word));
      numChunks_ = nLeft + nRight;
      size_ += other.size_;
      return true;
    }

    size_type left = 0;
    size_type right = 0;
    bool changed = false;
    while (right < nRight) {
      const std::uint32_t rBase = rArr[right].base;
      if (left < nLeft && lArr[left].base < rBase)
        left = gallopLowerBound(lArr, left, nLeft, rBase);
      if (left == nLeft || lArr[left].base > rBase)
        break;
      const Word &rw = rArr[right];
      Word &lw = lArr[left];
      const std::uint64_t a0 = rw.bits[0] & ~lw.bits[0];
      const std::uint64_t a1 = rw.bits[1] & ~lw.bits[1];
      const std::uint64_t a2 = rw.bits[2] & ~lw.bits[2];
      const std::uint64_t a3 = rw.bits[3] & ~lw.bits[3];
      if ((a0 | a1 | a2 | a3) != 0) {
        lw.bits[0] |= a0;
        lw.bits[1] |= a1;
        lw.bits[2] |= a2;
        lw.bits[3] |= a3;
        size_ += static_cast<std::uint32_t>(
            __builtin_popcountll(a0) + __builtin_popcountll(a1) +
            __builtin_popcountll(a2) + __builtin_popcountll(a3));
        changed = true;
      }
      ++left;
      ++right;
    }

    if (right == nRight)
      return changed;

    if (left == nLeft) {
      const std::uint32_t rem = nRight - static_cast<std::uint32_t>(right);
      ensureHeapCapacity(nLeft + rem, /*preserve=*/true);
      std::memcpy(st_.heap.data + nLeft, rArr + right,
                  static_cast<std::size_t>(rem) * sizeof(Word));
      for (size_type k = right; k < nRight; ++k)
        size_ += static_cast<std::uint32_t>(rArr[k].popcount());
      numChunks_ = nLeft + rem;
      return true;
    }

    auto &merged = mergeScratch();
    merged.clear();
    merged.reserve(nLeft + (nRight - right));
    merged.insert(merged.end(), lArr, lArr + left);
    while (left < nLeft || right < nRight) {
      if (right == nRight || (left < nLeft && lArr[left].base < rArr[right].base)) {
        merged.push_back(lArr[left++]);
      } else if (left == nLeft || rArr[right].base < lArr[left].base) {
        const Word &w = rArr[right++];
        merged.push_back(w);
        size_ += static_cast<std::uint32_t>(w.popcount());
      } else {
        const Word &lw = lArr[left];
        const Word &rw = rArr[right];
        const std::uint64_t a0 = rw.bits[0] & ~lw.bits[0];
        const std::uint64_t a1 = rw.bits[1] & ~lw.bits[1];
        const std::uint64_t a2 = rw.bits[2] & ~lw.bits[2];
        const std::uint64_t a3 = rw.bits[3] & ~lw.bits[3];
        merged.push_back(Word{lw.base,
                              {lw.bits[0] | rw.bits[0], lw.bits[1] | rw.bits[1],
                               lw.bits[2] | rw.bits[2],
                               lw.bits[3] | rw.bits[3]}});
        if ((a0 | a1 | a2 | a3) != 0) {
          size_ += static_cast<std::uint32_t>(
              __builtin_popcountll(a0) + __builtin_popcountll(a1) +
              __builtin_popcountll(a2) + __builtin_popcountll(a3));
        }
        ++left;
        ++right;
      }
    }
    const std::uint32_t outN = static_cast<std::uint32_t>(merged.size());
    ensureHeapCapacity(outN, /*preserve=*/false);
    std::memcpy(st_.heap.data, merged.data(),
                static_cast<std::size_t>(outN) * sizeof(Word));
    numChunks_ = outN;
    return true;
  }

  /// Insert a sorted (possibly duplicate) vector of IDs in linear chunk time.
  bool insertSorted(const std::vector<std::uint32_t> &sortedIds) {
    if (sortedIds.empty())
      return false;
    static thread_local SVFGNodeBS batch;
    batch.clear();
    for (std::uint32_t id : sortedIds) {
      const std::uint32_t w = wordIndex(id);
      const unsigned lane = subWord(id);
      const std::uint64_t m = bitMask(id);
      if (batch.numChunks_ > 0 && batch.data()[batch.numChunks_ - 1].base == w) {
        std::uint64_t &slot = batch.dataMut()[batch.numChunks_ - 1].bits[lane];
        if ((slot & m) == 0) {
          slot |= m;
          ++batch.size_;
        }
      } else {
        batch.appendChunk(makeSingleBitWord(w, lane, m));
        ++batch.size_;
      }
    }
    return unite(batch);
  }

  /// Union `other` into `*this` and populate `diff` with exactly the newly
  /// added bits (`other \ old(*this)`). Returns true iff `diff` is non-empty.
  bool uniteWithDiff(const SVFGNodeBS &other, SVFGNodeBS &diff) {
    diff.clear();
    if (other.size_ == 0 || this == &other)
      return false;
    if (size_ == 0) {
      *this = other;
      diff = other;
      return true;
    }
    const std::uint32_t nRight = other.numChunks_;
    const Word *rArr = other.data();
    if (nRight == 1) {
      const Word &ow = rArr[0];
      const std::uint32_t nLeft = numChunks_;
      Word *lArr = dataMut();
      Word &back = lArr[nLeft - 1];
      if (back.base == ow.base) {
        Word dw{ow.base,
                {ow.bits[0] & ~back.bits[0], ow.bits[1] & ~back.bits[1],
                 ow.bits[2] & ~back.bits[2], ow.bits[3] & ~back.bits[3]}};
        if (!dw.any())
          return false;
        back.bits[0] |= dw.bits[0];
        back.bits[1] |= dw.bits[1];
        back.bits[2] |= dw.bits[2];
        back.bits[3] |= dw.bits[3];
        const std::uint32_t pc = static_cast<std::uint32_t>(dw.popcount());
        size_ += pc;
        diff.appendChunk(dw);
        diff.size_ = pc;
        return true;
      }
      if (back.base < ow.base) {
        appendChunk(ow);
        size_ += other.size_;
        diff.appendChunk(ow);
        diff.size_ = other.size_;
        return true;
      }
      const size_type index =
          (nLeft == 1) ? 0 : lowerBound(lArr, nLeft, ow.base);
      if (index < nLeft && lArr[index].base == ow.base) {
        Word &lw = lArr[index];
        Word dw{ow.base,
                {ow.bits[0] & ~lw.bits[0], ow.bits[1] & ~lw.bits[1],
                 ow.bits[2] & ~lw.bits[2], ow.bits[3] & ~lw.bits[3]}};
        if (!dw.any())
          return false;
        lw.bits[0] |= dw.bits[0];
        lw.bits[1] |= dw.bits[1];
        lw.bits[2] |= dw.bits[2];
        lw.bits[3] |= dw.bits[3];
        const std::uint32_t pc = static_cast<std::uint32_t>(dw.popcount());
        size_ += pc;
        diff.appendChunk(dw);
        diff.size_ = pc;
        return true;
      }
      insertChunkAt(index, ow);
      size_ += other.size_;
      diff.appendChunk(ow);
      diff.size_ = other.size_;
      return true;
    }

    const std::uint32_t nLeft = numChunks_;
    Word *lArr = dataMut();

    if (lArr[nLeft - 1].base < rArr[0].base) {
      ensureHeapCapacity(nLeft + nRight, /*preserve=*/true);
      std::memcpy(st_.heap.data + nLeft, rArr,
                  static_cast<std::size_t>(nRight) * sizeof(Word));
      numChunks_ = nLeft + nRight;
      size_ += other.size_;
      diff = other;
      return true;
    }

    size_type left = 0;
    size_type right = 0;
    while (right < nRight) {
      const std::uint32_t rBase = rArr[right].base;
      if (left < nLeft && lArr[left].base < rBase)
        left = gallopLowerBound(lArr, left, nLeft, rBase);
      if (left == nLeft || lArr[left].base > rBase)
        break;
      const Word &rw = rArr[right];
      Word &lw = lArr[left];
      const std::uint64_t a0 = rw.bits[0] & ~lw.bits[0];
      const std::uint64_t a1 = rw.bits[1] & ~lw.bits[1];
      const std::uint64_t a2 = rw.bits[2] & ~lw.bits[2];
      const std::uint64_t a3 = rw.bits[3] & ~lw.bits[3];
      if ((a0 | a1 | a2 | a3) != 0) {
        lw.bits[0] |= a0;
        lw.bits[1] |= a1;
        lw.bits[2] |= a2;
        lw.bits[3] |= a3;
        const std::uint32_t pc = static_cast<std::uint32_t>(
            __builtin_popcountll(a0) + __builtin_popcountll(a1) +
            __builtin_popcountll(a2) + __builtin_popcountll(a3));
        size_ += pc;
        diff.appendChunk(Word{rBase, {a0, a1, a2, a3}});
        diff.size_ += pc;
      }
      ++left;
      ++right;
    }

    if (right == nRight)
      return diff.size_ != 0;

    if (left == nLeft) {
      const std::uint32_t rem = nRight - static_cast<std::uint32_t>(right);
      ensureHeapCapacity(nLeft + rem, /*preserve=*/true);
      std::memcpy(st_.heap.data + nLeft, rArr + right,
                  static_cast<std::size_t>(rem) * sizeof(Word));
      const std::uint32_t oldDiffN = diff.numChunks_;
      if (oldDiffN + rem >= 2) {
        diff.ensureHeapCapacity(oldDiffN + rem, /*preserve=*/true);
        std::memcpy(diff.st_.heap.data + oldDiffN, rArr + right,
                    static_cast<std::size_t>(rem) * sizeof(Word));
        diff.numChunks_ = oldDiffN + rem;
      } else {
        diff.appendChunk(rArr[right]);
      }
      std::uint32_t addedPc = 0;
      for (size_type k = right; k < nRight; ++k)
        addedPc += static_cast<std::uint32_t>(rArr[k].popcount());
      size_ += addedPc;
      diff.size_ += addedPc;
      numChunks_ = nLeft + rem;
      return true;
    }

    auto &merged = mergeScratch();
    merged.clear();
    merged.reserve(nLeft + (nRight - right));
    merged.insert(merged.end(), lArr, lArr + left);
    while (left < nLeft || right < nRight) {
      if (right == nRight || (left < nLeft && lArr[left].base < rArr[right].base)) {
        merged.push_back(lArr[left++]);
      } else if (left == nLeft || rArr[right].base < lArr[left].base) {
        const Word &w = rArr[right++];
        merged.push_back(w);
        const std::uint32_t pc = static_cast<std::uint32_t>(w.popcount());
        size_ += pc;
        diff.appendChunk(w);
        diff.size_ += pc;
      } else {
        const Word &lw = lArr[left];
        const Word &rw = rArr[right];
        const std::uint64_t a0 = rw.bits[0] & ~lw.bits[0];
        const std::uint64_t a1 = rw.bits[1] & ~lw.bits[1];
        const std::uint64_t a2 = rw.bits[2] & ~lw.bits[2];
        const std::uint64_t a3 = rw.bits[3] & ~lw.bits[3];
        merged.push_back(Word{lw.base,
                              {lw.bits[0] | rw.bits[0], lw.bits[1] | rw.bits[1],
                               lw.bits[2] | rw.bits[2],
                               lw.bits[3] | rw.bits[3]}});
        if ((a0 | a1 | a2 | a3) != 0) {
          const std::uint32_t pc = static_cast<std::uint32_t>(
              __builtin_popcountll(a0) + __builtin_popcountll(a1) +
              __builtin_popcountll(a2) + __builtin_popcountll(a3));
          size_ += pc;
          diff.appendChunk(Word{lw.base, {a0, a1, a2, a3}});
          diff.size_ += pc;
        }
        ++left;
        ++right;
      }
    }
    const std::uint32_t outN = static_cast<std::uint32_t>(merged.size());
    ensureHeapCapacity(outN, /*preserve=*/false);
    std::memcpy(st_.heap.data, merged.data(),
                static_cast<std::size_t>(outN) * sizeof(Word));
    numChunks_ = outN;
    return true;
  }

  /// Return true iff every bit in `*this` is also present in `other`.
  bool isSubsetOf(const SVFGNodeBS &other) const noexcept {
    if (size_ == 0 || this == &other)
      return true;
    if (size_ > other.size_ || numChunks_ > other.numChunks_)
      return false;
    const std::uint32_t nLeft = numChunks_;
    const std::uint32_t nRight = other.numChunks_;
    const Word *lArr = data();
    const Word *rArr = other.data();
    if (lArr[0].base < rArr[0].base ||
        lArr[nLeft - 1].base > rArr[nRight - 1].base)
      return false;
    size_type right = 0;
    for (size_type left = 0; left < nLeft; ++left) {
      const Word &lw = lArr[left];
      if (right < nRight && rArr[right].base < lw.base)
        right = gallopLowerBound(rArr, right, nRight, lw.base);
      if (right == nRight || rArr[right].base != lw.base)
        return false;
      const Word &rw = rArr[right];
      if (((lw.bits[0] & ~rw.bits[0]) | (lw.bits[1] & ~rw.bits[1]) |
           (lw.bits[2] & ~rw.bits[2]) | (lw.bits[3] & ~rw.bits[3])) != 0)
        return false;
      ++right;
    }
    return true;
  }

  /// Invoke `fn(id)` for every element in `*this \ exclude` in ascending order
  /// without allocating a temporary bitset.
  template <typename Fn>
  void forEachNotIn(const SVFGNodeBS &exclude, Fn &&fn) const {
    if (size_ == 0 || this == &exclude)
      return;
    const std::uint32_t nLeft = numChunks_;
    const std::uint32_t nRight = exclude.numChunks_;
    const Word *lArr = data();
    const Word *rArr = exclude.data();
    const bool disjoint = (nRight == 0 || lArr[nLeft - 1].base < rArr[0].base ||
                           lArr[0].base > rArr[nRight - 1].base);
    size_type right = 0;
    for (size_type left = 0; left < nLeft; ++left) {
      const Word &lw = lArr[left];
      std::uint64_t d0 = lw.bits[0];
      std::uint64_t d1 = lw.bits[1];
      std::uint64_t d2 = lw.bits[2];
      std::uint64_t d3 = lw.bits[3];
      if (!disjoint) {
        if (right < nRight && rArr[right].base < lw.base)
          right = gallopLowerBound(rArr, right, nRight, lw.base);
        if (right < nRight && rArr[right].base == lw.base) {
          const Word &rw = rArr[right];
          d0 &= ~rw.bits[0];
          d1 &= ~rw.bits[1];
          d2 &= ~rw.bits[2];
          d3 &= ~rw.bits[3];
          ++right;
        }
      }
      if ((d0 | d1 | d2 | d3) == 0)
        continue;
      const std::uint32_t baseShift = lw.base << 8;
      const std::uint64_t lanes[4] = {d0, d1, d2, d3};
      for (unsigned k = 0; k < 4U; ++k) {
        std::uint64_t diffBits = lanes[k];
        if (!diffBits)
          continue;
        const std::uint32_t laneShift = baseShift | (k << 6);
        while (diffBits != 0) {
          const unsigned bit = static_cast<unsigned>(__builtin_ctzll(diffBits));
          fn(laneShift | bit);
          diffBits &= diffBits - 1;
        }
      }
    }
  }

  /// Return true iff `*this ∩ other` is non-empty, exiting on the first
  /// overlapping chunk word.
  bool intersects(const SVFGNodeBS &other) const noexcept {
    if (size_ == 0 || other.size_ == 0)
      return false;
    const std::uint32_t lSize = numChunks_;
    const std::uint32_t rSize = other.numChunks_;
    const Word *lArr = data();
    const Word *rArr = other.data();
    if (lArr[lSize - 1].base < rArr[0].base ||
        rArr[rSize - 1].base < lArr[0].base)
      return false;
    size_type left = 0, right = 0;
    while (left < lSize && right < rSize) {
      const Word &lw = lArr[left];
      const Word &rw = rArr[right];
      if (lw.base < rw.base) {
        left = gallopLowerBound(lArr, left + 1, lSize, rw.base);
      } else if (rw.base < lw.base) {
        right = gallopLowerBound(rArr, right + 1, rSize, lw.base);
      } else {
        if (((lw.bits[0] & rw.bits[0]) | (lw.bits[1] & rw.bits[1]) |
             (lw.bits[2] & rw.bits[2]) | (lw.bits[3] & rw.bits[3])) != 0)
          return true;
        ++left;
        ++right;
      }
    }
    return false;
  }

  /// Invoke `fn(id)` for every `id` in `*this ∩ other` in ascending order,
  /// scanning only matching chunks with galloping skips.
  template <typename Fn>
  void forEachIn(const SVFGNodeBS &other, Fn &&fn) const {
    if (size_ == 0 || other.size_ == 0)
      return;
    const std::uint32_t lSize = numChunks_;
    const std::uint32_t rSize = other.numChunks_;
    const Word *lArr = data();
    const Word *rArr = other.data();
    if (lArr[lSize - 1].base < rArr[0].base ||
        rArr[rSize - 1].base < lArr[0].base)
      return;
    size_type left = 0, right = 0;
    while (left < lSize && right < rSize) {
      const Word &lw = lArr[left];
      const Word &rw = rArr[right];
      if (lw.base < rw.base) {
        left = gallopLowerBound(lArr, left + 1, lSize, rw.base);
      } else if (rw.base < lw.base) {
        right = gallopLowerBound(rArr, right + 1, rSize, lw.base);
      } else {
        const std::uint64_t a0 = lw.bits[0] & rw.bits[0];
        const std::uint64_t a1 = lw.bits[1] & rw.bits[1];
        const std::uint64_t a2 = lw.bits[2] & rw.bits[2];
        const std::uint64_t a3 = lw.bits[3] & rw.bits[3];
        if ((a0 | a1 | a2 | a3) != 0) {
          const std::uint32_t baseShift = lw.base << 8;
          const std::uint64_t lanes[4] = {a0, a1, a2, a3};
          for (unsigned k = 0; k < 4U; ++k) {
            std::uint64_t cur = lanes[k];
            if (!cur)
              continue;
            const std::uint32_t laneShift = baseShift | (k << 6);
            while (cur != 0) {
              const unsigned bit = static_cast<unsigned>(__builtin_ctzll(cur));
              fn(laneShift | bit);
              cur &= cur - 1;
            }
          }
        }
        ++left;
        ++right;
      }
    }
  }

  /// Fast 64-bit content hash for hash-consing identical bitsets.
  std::uint64_t hash() const noexcept {
    std::uint64_t h = static_cast<std::uint64_t>(size_) * 0x9e3779b97f4a7c15ULL;
    const std::uint32_t n = numChunks_;
    const Word *arr = data();
    for (std::uint32_t i = 0; i < n; ++i) {
      const Word &w = arr[i];
      std::uint64_t k = static_cast<std::uint64_t>(w.base) * 0xbf58476d1ce4e5b9ULL;
      k ^= w.bits[0] * 0x9e3779b97f4a7c15ULL;
      k ^= (w.bits[1] << 7) | (w.bits[1] >> 57);
      k ^= (w.bits[2] << 19) | (w.bits[2] >> 45);
      k ^= (w.bits[3] << 31) | (w.bits[3] >> 33);
      k ^= k >> 30;
      k *= 0xbf58476d1ce4e5b9ULL;
      k ^= k >> 27;
      h ^= k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    return h;
  }

  /// Remove a set in chunk-linear/galloping time, retaining sorted nonempty
  /// chunks.
  bool subtract(const SVFGNodeBS &other) noexcept {
    if (size_ == 0 || other.size_ == 0)
      return false;
    if (this == &other) {
      clear();
      return true;
    }
    const std::uint32_t nLeft = numChunks_;
    const std::uint32_t nRight = other.numChunks_;
    Word *lArr = dataMut();
    const Word *rArr = other.data();
    if (lArr[nLeft - 1].base < rArr[0].base ||
        lArr[0].base > rArr[nRight - 1].base)
      return false;
    size_type right = 0, output = 0;
    std::uint32_t removed = 0;
    for (size_type left = 0; left < nLeft; ++left) {
      Word cur = lArr[left];
      if (right < nRight && rArr[right].base < cur.base)
        right = gallopLowerBound(rArr, right, nRight, cur.base);
      if (right < nRight && rArr[right].base == cur.base) {
        const Word &rw = rArr[right];
        const std::uint64_t r0 = cur.bits[0] & rw.bits[0];
        const std::uint64_t r1 = cur.bits[1] & rw.bits[1];
        const std::uint64_t r2 = cur.bits[2] & rw.bits[2];
        const std::uint64_t r3 = cur.bits[3] & rw.bits[3];
        if ((r0 | r1 | r2 | r3) != 0) {
          removed += static_cast<std::uint32_t>(
              __builtin_popcountll(r0) + __builtin_popcountll(r1) +
              __builtin_popcountll(r2) + __builtin_popcountll(r3));
          cur.bits[0] ^= r0;
          cur.bits[1] ^= r1;
          cur.bits[2] ^= r2;
          cur.bits[3] ^= r3;
        }
        ++right;
      }
      if (cur.any())
        lArr[output++] = cur;
    }
    numChunks_ = static_cast<std::uint32_t>(output);
    size_ -= removed;
    return removed != 0;
  }

  /// Hint overload required by std::insert_iterator (std::inserter).
  iterator insert(iterator, std::uint32_t id) { return insert(id).first; }
  template <typename InputIt> void insert(InputIt first, InputIt last) {
    for (; first != last; ++first)
      insertBit(static_cast<std::uint32_t>(*first));
  }

  void insert(std::initializer_list<std::uint32_t> values) {
    for (std::uint32_t value : values)
      insertBit(value);
  }

  /// Rewrite every element through @p map and re-canonicalise the chunk layout.
  template <typename MapFn> void remapIds(MapFn &&map) {
    if (numChunks_ == 0)
      return;
    std::vector<std::uint32_t> ids;
    ids.reserve(size_);
    for (std::uint32_t id : *this)
      ids.push_back(static_cast<std::uint32_t>(map(id)));
    clear();
    for (std::uint32_t id : ids)
      insertBit(id);
  }

  size_type erase(std::uint32_t id) noexcept {
    const std::uint32_t n = numChunks_;
    if (n == 0)
      return 0;
    const std::uint32_t w = wordIndex(id);
    Word *arr = dataMut();
    const size_type index =
        (n == 1) ? (arr[0].base == w ? 0 : 1) : lowerBound(arr, n, w);
    if (index == n || arr[index].base != w)
      return 0;
    const unsigned lane = subWord(id);
    const std::uint64_t mask = bitMask(id);
    if ((arr[index].bits[lane] & mask) == 0)
      return 0;
    arr[index].bits[lane] &= ~mask;
    if (!arr[index].any()) {
      const size_type tail = n - index - 1;
      if (tail > 0)
        std::memmove(arr + index, arr + index + 1, tail * sizeof(Word));
      numChunks_ = n - 1;
    }
    --size_;
    return 1;
  }

  iterator erase(iterator position) {
    if (position == end())
      return end();
    iterator next = position;
    ++next;
    const std::uint32_t nextValue = (next == end()) ? 0 : *next;
    const bool hasNext = next != end();
    erase(*position);
    return hasNext ? find(nextValue) : end();
  }

  void clear() noexcept {
    numChunks_ = 0;
    size_ = 0;
  }

  void swap(SVFGNodeBS &other) noexcept {
    SVFGNodeBS tmp(std::move(*this));
    *this = std::move(other);
    other = std::move(tmp);
  }

  //===------------------------------------------------------------------===//
  // Comparison
  //===------------------------------------------------------------------===//

  friend bool operator==(const SVFGNodeBS &lhs,
                         const SVFGNodeBS &rhs) noexcept {
    if (lhs.size_ != rhs.size_ || lhs.numChunks_ != rhs.numChunks_)
      return false;
    const std::uint32_t n = lhs.numChunks_;
    if (n == 0)
      return true;
    const Word *la = lhs.data();
    const Word *ra = rhs.data();
    for (std::uint32_t i = 0; i < n; ++i) {
      if (!(la[i] == ra[i]))
        return false;
    }
    return true;
  }
  friend bool operator!=(const SVFGNodeBS &lhs,
                         const SVFGNodeBS &rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const SVFGNodeBS &lhs, const SVFGNodeBS &rhs) noexcept {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                        rhs.end());
  }
};

} // namespace analysis
} // namespace lotus
