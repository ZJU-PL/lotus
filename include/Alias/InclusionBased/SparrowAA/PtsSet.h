/**
 * @file PtsSet.h
 * @brief Copy-on-Write flat contiguous chunked-bitset points-to set.
 *
 * ## Design
 *
 * `AndersPtsSet` stores an intrusive reference-counted pointer to a contiguous
 * array of sorted, non-empty 512-bit chunks (`uint32_t idx` +
 * `uint64_t words[8]`).
 *
 * Key properties:
 * - **8-byte inline footprint**: empty sets are a single `nullptr`.
 * - **Copy-on-Write + superset sharing**: copying a points-to set (e.g.
 *   `oldPtsGraph[u] = ptsGraph[u]`, propagating into an empty target, or
 *   unioning a smaller set with a superset) bumps a refcount with zero memory
 *   allocation and zero byte copying.
 * - **O(1) pointer-equality fast paths**: `unionWith`, `setDifference`,
 *   `intersectAssign`, and `operator==` short-circuit immediately when two sets
 *   share the same underlying buffer.
 * - **Cache-friendly linear scans**: when sets do differ, chunks live in a
 *   single contiguous allocation instead of `llvm::SparseBitVector`'s
 *   per-chunk heap-allocated doubly-linked list.
 *
 * @note `getSize()` counts set bits across chunks. Prefer `isEmpty()` for
 *       empty checks.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <new>
#include <type_traits>
#include <vector>

class AndersPtsSet {
private:
  static constexpr unsigned kWordBits = 64;
  static constexpr unsigned kWordsPerChunk = 8; // 512 bits per chunk
  static constexpr unsigned kChunkBits = kWordBits * kWordsPerChunk;

  struct Chunk {
    uint32_t idx = 0;
    uint32_t pad = 0;
    uint64_t words[kWordsPerChunk] = {0, 0, 0, 0, 0, 0, 0, 0};

    bool operator==(const Chunk &other) const {
      if (idx != other.idx)
        return false;
      for (unsigned w = 0; w < kWordsPerChunk; ++w)
        if (words[w] != other.words[w])
          return false;
      return true;
    }
  };
  static_assert(std::is_trivially_copyable<Chunk>::value,
                "Chunk must be trivially copyable");

  struct alignas(8) Buf {
    mutable std::atomic<uint32_t> refCount;
    uint32_t size;
    uint32_t cap;
    uint32_t pad;

    Chunk *chunks() { return reinterpret_cast<Chunk *>(this + 1); }
    const Chunk *chunks() const {
      return reinterpret_cast<const Chunk *>(this + 1);
    }
  };
  static_assert(sizeof(Buf) == 16 && (sizeof(Buf) % alignof(Chunk) == 0),
                "Buf header must preserve Chunk alignment");

  // `buf == nullptr` or `buf->size == 0` represents the empty set.
  // Note: `buf->size == 0` can occur only when `clear()` preserves capacity on
  // a uniquely-owned scratch buffer (such as the solver's `delta`).
  Buf *buf = nullptr;

  static Buf *allocBuf(uint32_t cap) {
    assert(cap > 0);
    void *mem =
        ::operator new(sizeof(Buf) + static_cast<size_t>(cap) * sizeof(Chunk));
    return new (mem) Buf{1u, 0u, cap, 0u};
  }

  static void retain(Buf *b) noexcept {
    if (b)
      b->refCount.fetch_add(1u, std::memory_order_relaxed);
  }

  static void release(Buf *b) noexcept {
    if (b && b->refCount.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
      b->~Buf();
      ::operator delete(b);
    }
  }

  static inline unsigned ctz64(uint64_t value) {
#if defined(__clang__) || defined(__GNUC__)
    return static_cast<unsigned>(__builtin_ctzll(value));
#else
    unsigned n = 0;
    while ((value & 1u) == 0) {
      value >>= 1;
      ++n;
    }
    return n;
#endif
  }

  static inline unsigned popcount64(uint64_t value) {
#if defined(__clang__) || defined(__GNUC__)
    return static_cast<unsigned>(__builtin_popcountll(value));
#else
    unsigned n = 0;
    while (value) {
      value &= (value - 1);
      ++n;
    }
    return n;
#endif
  }

  static uint32_t findChunkIn(const Chunk *arr, uint32_t n, uint32_t chunkIdx) {
    const Chunk *it =
        std::lower_bound(arr, arr + n, chunkIdx,
                         [](const Chunk &c, uint32_t id) { return c.idx < id; });
    return static_cast<uint32_t>(it - arr);
  }

  static bool chunkSupersetOf(const Chunk &super, const Chunk &sub) {
    for (unsigned w = 0; w < kWordsPerChunk; ++w) {
      if ((super.words[w] & sub.words[w]) != sub.words[w])
        return false;
    }
    return true;
  }

  // Ensure `buf` is uniquely owned with capacity at least `minCap`.
  void ensureUniqueForGrow(uint32_t minCap) {
    if (!buf) {
      buf = allocBuf(std::max(minCap, 1u));
      return;
    }
    bool unique = (buf->refCount.load(std::memory_order_acquire) == 1u);
    if (unique && buf->cap >= minCap)
      return;
    uint32_t newCap = std::max(minCap, buf->cap ? buf->cap * 2u : 1u);
    Buf *nb = allocBuf(newCap);
    nb->size = buf->size;
    if (buf->size > 0) {
      std::memcpy(nb->chunks(), buf->chunks(),
                  static_cast<size_t>(buf->size) * sizeof(Chunk));
    }
    release(buf);
    buf = nb;
  }

public:
  AndersPtsSet() = default;

  AndersPtsSet(const AndersPtsSet &other) noexcept : buf(other.buf) {
    if (buf && buf->size == 0) {
      buf = nullptr;
    } else {
      retain(buf);
    }
  }

  AndersPtsSet(AndersPtsSet &&other) noexcept : buf(other.buf) {
    other.buf = nullptr;
  }

  AndersPtsSet &operator=(const AndersPtsSet &other) noexcept {
    Buf *src = (other.buf && other.buf->size > 0) ? other.buf : nullptr;
    if (buf != src) {
      retain(src);
      release(buf);
      buf = src;
    }
    return *this;
  }

  AndersPtsSet &operator=(AndersPtsSet &&other) noexcept {
    if (this != &other) {
      release(buf);
      buf = other.buf;
      other.buf = nullptr;
    }
    return *this;
  }

  ~AndersPtsSet() { release(buf); }

  class iterator {
  public:
    using value_type = unsigned;
    using difference_type = std::ptrdiff_t;
    using pointer = const unsigned *;
    using reference = unsigned;
    using iterator_category = std::forward_iterator_tag;

    iterator() = default;

    unsigned operator*() const { return current; }

    iterator &operator++() {
      advance();
      return *this;
    }

    iterator operator++(int) {
      iterator tmp = *this;
      advance();
      return tmp;
    }

    bool operator==(const iterator &other) const {
      if (atEnd && other.atEnd)
        return true;
      return atEnd == other.atEnd && chunks == other.chunks &&
             chunkPos == other.chunkPos && wordPos == other.wordPos &&
             curWord == other.curWord;
    }

    bool operator!=(const iterator &other) const { return !(*this == other); }

  private:
    friend class AndersPtsSet;

    explicit iterator(const Chunk *c, uint32_t n, bool end)
        : chunks(c), numChunks(n), atEnd(end || !c || n == 0) {
      if (!atEnd)
        findFirst();
    }

    void findFirst() {
      while (chunkPos < numChunks) {
        const Chunk &ch = chunks[chunkPos];
        for (wordPos = 0; wordPos < kWordsPerChunk; ++wordPos) {
          curWord = ch.words[wordPos];
          if (curWord != 0) {
            current = ch.idx * kChunkBits + wordPos * kWordBits + ctz64(curWord);
            return;
          }
        }
        ++chunkPos;
      }
      atEnd = true;
    }

    void advance() {
      if (atEnd)
        return;
      curWord &= (curWord - 1);
      while (curWord == 0) {
        ++wordPos;
        if (wordPos >= kWordsPerChunk) {
          ++chunkPos;
          wordPos = 0;
          if (chunkPos >= numChunks) {
            atEnd = true;
            return;
          }
        }
        curWord = chunks[chunkPos].words[wordPos];
      }
      current = chunks[chunkPos].idx * kChunkBits + wordPos * kWordBits +
                ctz64(curWord);
    }

    const Chunk *chunks = nullptr;
    uint32_t numChunks = 0;
    uint32_t chunkPos = 0;
    unsigned wordPos = 0;
    uint64_t curWord = 0;
    unsigned current = 0;
    bool atEnd = true;
  };

  bool has(unsigned idx) {
    return static_cast<const AndersPtsSet &>(*this).has(idx);
  }

  bool has(unsigned idx) const {
    if (!buf || buf->size == 0)
      return false;
    const Chunk *arr = buf->chunks();
    const uint32_t n = buf->size;
    const uint32_t chunkIdx = idx / kChunkBits;
    const unsigned off = idx % kChunkBits;
    const unsigned word = off >> 6;
    const uint64_t mask = uint64_t{1} << (off & 63u);

    // O(1) fast path for chunk 0 (e.g., UniversalObj / NullObj checks).
    if (chunkIdx == 0)
      return arr[0].idx == 0 && (arr[0].words[word] & mask) != 0;

    const uint32_t pos = findChunkIn(arr, n, chunkIdx);
    return pos < n && arr[pos].idx == chunkIdx &&
           (arr[pos].words[word] & mask) != 0;
  }

  /// @brief Insert @p idx into the set. @return `true` if the set changed.
  bool insert(unsigned idx) {
    const uint32_t chunkIdx = idx / kChunkBits;
    const unsigned off = idx % kChunkBits;
    const unsigned word = off >> 6;
    const uint64_t mask = uint64_t{1} << (off & 63u);

    if (!buf || buf->size == 0) {
      ensureUniqueForGrow(1u);
      Chunk c;
      c.idx = chunkIdx;
      c.words[word] = mask;
      buf->chunks()[0] = c;
      buf->size = 1u;
      return true;
    }

    const Chunk *arr = buf->chunks();
    const uint32_t n = buf->size;

    // Fast path: monotonically ascending append or last-chunk update.
    if (arr[n - 1].idx < chunkIdx) {
      ensureUniqueForGrow(n + 1u);
      Chunk c;
      c.idx = chunkIdx;
      c.words[word] = mask;
      buf->chunks()[n] = c;
      buf->size = n + 1u;
      return true;
    }
    if (arr[n - 1].idx == chunkIdx) {
      if ((arr[n - 1].words[word] & mask) != 0)
        return false;
      ensureUniqueForGrow(n);
      buf->chunks()[n - 1].words[word] |= mask;
      return true;
    }

    const uint32_t pos = findChunkIn(arr, n, chunkIdx);
    if (pos < n && arr[pos].idx == chunkIdx) {
      if ((arr[pos].words[word] & mask) != 0)
        return false;
      ensureUniqueForGrow(n);
      buf->chunks()[pos].words[word] |= mask;
      return true;
    }

    ensureUniqueForGrow(n + 1u);
    Chunk *dst = buf->chunks();
    std::memmove(dst + pos + 1, dst + pos,
                 static_cast<size_t>(n - pos) * sizeof(Chunk));
    Chunk c;
    c.idx = chunkIdx;
    c.words[word] = mask;
    dst[pos] = c;
    buf->size = n + 1u;
    return true;
  }

  /// @brief Return `true` if this set is a superset of @p other.
  bool contains(const AndersPtsSet &other) const {
    if (other.isEmpty() || buf == other.buf)
      return true;
    if (isEmpty())
      return false;
    const uint32_t n = buf->size, m = other.buf->size;
    if (n < m)
      return false;
    const Chunk *a = buf->chunks();
    const Chunk *b = other.buf->chunks();
    if (m == 1 && n > 8) {
      uint32_t pos = findChunkIn(a, n, b[0].idx);
      return pos < n && a[pos].idx == b[0].idx && chunkSupersetOf(a[pos], b[0]);
    }
    uint32_t i = 0, j = 0;
    while (i < n && j < m) {
      if (a[i].idx < b[j].idx) {
        ++i;
      } else if (a[i].idx > b[j].idx) {
        return false;
      } else {
        if (!chunkSupersetOf(a[i], b[j]))
          return false;
        ++i;
        ++j;
      }
    }
    return j == m;
  }

  /// @brief Return `true` if this set and @p other share at least one element.
  bool intersectWith(const AndersPtsSet &other) const {
    if (isEmpty() || other.isEmpty())
      return false;
    if (buf == other.buf)
      return true;
    const Chunk *a = buf->chunks();
    const Chunk *b = other.buf->chunks();
    const uint32_t n = buf->size, m = other.buf->size;
    uint32_t i = 0, j = 0;
    while (i < n && j < m) {
      if (a[i].idx < b[j].idx) {
        ++i;
      } else if (a[i].idx > b[j].idx) {
        ++j;
      } else {
        for (unsigned w = 0; w < kWordsPerChunk; ++w) {
          if ((a[i].words[w] & b[j].words[w]) != 0)
            return true;
        }
        ++i;
        ++j;
      }
    }
    return false;
  }

  /// @brief Union @p other into this set. @return `true` if the set changed.
  bool unionWith(const AndersPtsSet &other) {
    if (buf == other.buf || other.isEmpty())
      return false;
    if (isEmpty()) {
      retain(other.buf);
      release(buf);
      buf = other.buf;
      return true;
    }

    const Chunk *a = buf->chunks();
    const uint32_t n = buf->size;
    const Chunk *b = other.buf->chunks();
    const uint32_t m = other.buf->size;

    // Fast O(log m) singleton-in-superset check (very common when a huge
    // shared points-to set sweeps across singleton pointers).
    if (n == 1 && m > 8) {
      uint32_t pos = findChunkIn(b, m, a[0].idx);
      if (pos < m && b[pos].idx == a[0].idx && chunkSupersetOf(b[pos], a[0])) {
        retain(other.buf);
        release(buf);
        buf = other.buf;
        return true;
      }
    } else if (m == 1 && n > 8) {
      uint32_t pos = findChunkIn(a, n, b[0].idx);
      if (pos < n && a[pos].idx == b[0].idx && chunkSupersetOf(a[pos], b[0])) {
        return false;
      }
    } else {
      // Single-pass subset/superset classification: avoids allocating whenever
      // other ⊆ this (no-op) or this ⊆ other (zero-copy buffer sharing!).
      bool aContainsB = (n >= m);
      bool bContainsA = (m >= n);
      uint32_t i = 0, j = 0;
      while (i < n && j < m) {
        if (a[i].idx < b[j].idx) {
          bContainsA = false;
          ++i;
        } else if (b[j].idx < a[i].idx) {
          aContainsB = false;
          ++j;
        } else {
          for (unsigned w = 0; w < kWordsPerChunk; ++w) {
            uint64_t aw = a[i].words[w];
            uint64_t bw = b[j].words[w];
            if ((aw & bw) != bw)
              aContainsB = false;
            if ((aw & bw) != aw)
              bContainsA = false;
          }
          ++i;
          ++j;
        }
        if (!aContainsB && !bContainsA)
          break;
      }
      if (i < n)
        bContainsA = false;
      if (j < m)
        aContainsB = false;

      if (aContainsB) {
        if (bContainsA && other.buf->refCount.load(std::memory_order_relaxed) >=
                              buf->refCount.load(std::memory_order_relaxed)) {
          retain(other.buf);
          release(buf);
          buf = other.buf;
        }
        return false;
      }
      if (bContainsA) {
        retain(other.buf);
        release(buf);
        buf = other.buf;
        return true;
      }
    }

    // Uniquely-owned fast in-place OR path.
    if (buf->refCount.load(std::memory_order_acquire) == 1u) {
      Chunk *dst = buf->chunks();
      uint32_t i = 0, j = 0;
      bool changed = false;
      bool needMerge = false;
      while (i < n && j < m) {
        if (dst[i].idx < b[j].idx) {
          ++i;
        } else if (dst[i].idx == b[j].idx) {
          uint64_t diff = 0;
          for (unsigned w = 0; w < kWordsPerChunk; ++w) {
            uint64_t oldW = dst[i].words[w];
            uint64_t newW = oldW | b[j].words[w];
            dst[i].words[w] = newW;
            diff |= (oldW ^ newW);
          }
          if (diff != 0)
            changed = true;
          ++i;
          ++j;
        } else {
          needMerge = true;
          break;
        }
      }
      if (!needMerge) {
        if (j == m)
          return changed;
        uint32_t rem = m - j;
        if (n + rem <= buf->cap) {
          std::memcpy(dst + n, b + j, static_cast<size_t>(rem) * sizeof(Chunk));
          buf->size = n + rem;
          return true;
        }
        Buf *nb = allocBuf(n + rem);
        std::memcpy(nb->chunks(), dst, static_cast<size_t>(n) * sizeof(Chunk));
        std::memcpy(nb->chunks() + n, b + j,
                    static_cast<size_t>(rem) * sizeof(Chunk));
        nb->size = n + rem;
        release(buf);
        buf = nb;
        return true;
      }

      // Merge prefix dst[0..i-1] (already ORed with b[0..j-1]) + remaining.
      static thread_local std::vector<Chunk> scratch;
      scratch.clear();
      scratch.reserve(n + (m - j));
      scratch.insert(scratch.end(), dst, dst + i);
      while (i < n && j < m) {
        if (dst[i].idx < b[j].idx) {
          scratch.push_back(dst[i++]);
        } else if (b[j].idx < dst[i].idx) {
          scratch.push_back(b[j++]);
        } else {
          Chunk c = dst[i++];
          const Chunk &o = b[j++];
          for (unsigned w = 0; w < kWordsPerChunk; ++w)
            c.words[w] |= o.words[w];
          scratch.push_back(c);
        }
      }
      while (i < n)
        scratch.push_back(dst[i++]);
      while (j < m)
        scratch.push_back(b[j++]);
      uint32_t newSize = static_cast<uint32_t>(scratch.size());
      if (newSize <= buf->cap) {
        std::memcpy(dst, scratch.data(),
                    static_cast<size_t>(newSize) * sizeof(Chunk));
        buf->size = newSize;
      } else {
        Buf *nb = allocBuf(newSize);
        std::memcpy(nb->chunks(), scratch.data(),
                    static_cast<size_t>(newSize) * sizeof(Chunk));
        nb->size = newSize;
        release(buf);
        buf = nb;
      }
      return true;
    }

    // Shared buffer where neither set contains the other: merge into a new
    // exact-sized buffer (zero capacity slack).
    static thread_local std::vector<Chunk> scratch;
    scratch.clear();
    scratch.reserve(n + m);
    uint32_t i = 0, j = 0;
    while (i < n && j < m) {
      if (a[i].idx < b[j].idx) {
        scratch.push_back(a[i++]);
      } else if (b[j].idx < a[i].idx) {
        scratch.push_back(b[j++]);
      } else {
        Chunk c = a[i++];
        const Chunk &o = b[j++];
        for (unsigned w = 0; w < kWordsPerChunk; ++w)
          c.words[w] |= o.words[w];
        scratch.push_back(c);
      }
    }
    while (i < n)
      scratch.push_back(a[i++]);
    while (j < m)
      scratch.push_back(b[j++]);
    uint32_t newSize = static_cast<uint32_t>(scratch.size());
    Buf *nb = allocBuf(newSize);
    std::memcpy(nb->chunks(), scratch.data(),
                static_cast<size_t>(newSize) * sizeof(Chunk));
    nb->size = newSize;
    release(buf);
    buf = nb;
    return true;
  }

  /// @brief Intersect this set in-place with @p other. @return `true` if changed.
  bool intersectAssign(const AndersPtsSet &other) {
    if (buf == other.buf || isEmpty())
      return false;
    if (other.isEmpty()) {
      clearAndRelease();
      return true;
    }
    if (other.contains(*this))
      return false;
    if (contains(other)) {
      retain(other.buf);
      release(buf);
      buf = other.buf;
      return true;
    }

    const Chunk *a = buf->chunks();
    const Chunk *b = other.buf->chunks();
    const uint32_t n = buf->size, m = other.buf->size;

    if (buf->refCount.load(std::memory_order_acquire) == 1u) {
      Chunk *dst = buf->chunks();
      uint32_t i = 0, j = 0, out = 0;
      bool changed = false;
      while (i < n && j < m) {
        if (dst[i].idx < b[j].idx) {
          changed = true;
          ++i;
        } else if (b[j].idx < dst[i].idx) {
          ++j;
        } else {
          uint64_t any = 0;
          uint64_t diff = 0;
          dst[out].idx = dst[i].idx;
          dst[out].pad = 0;
          for (unsigned w = 0; w < kWordsPerChunk; ++w) {
            uint64_t nw = dst[i].words[w] & b[j].words[w];
            diff |= (dst[i].words[w] ^ nw);
            dst[out].words[w] = nw;
            any |= nw;
          }
          if (diff != 0)
            changed = true;
          if (any != 0)
            ++out;
          ++i;
          ++j;
        }
      }
      if (i < n)
        changed = true;
      buf->size = out;
      if (out == 0)
        clearAndRelease();
      return changed;
    }

    static thread_local std::vector<Chunk> scratch;
    scratch.clear();
    scratch.reserve(std::min(n, m));
    uint32_t i = 0, j = 0;
    while (i < n && j < m) {
      if (a[i].idx < b[j].idx) {
        ++i;
      } else if (b[j].idx < a[i].idx) {
        ++j;
      } else {
        Chunk c;
        c.idx = a[i].idx;
        uint64_t any = 0;
        for (unsigned w = 0; w < kWordsPerChunk; ++w) {
          uint64_t nw = a[i].words[w] & b[j].words[w];
          c.words[w] = nw;
          any |= nw;
        }
        if (any != 0)
          scratch.push_back(c);
        ++i;
        ++j;
      }
    }
    release(buf);
    if (scratch.empty()) {
      buf = nullptr;
    } else {
      uint32_t newSize = static_cast<uint32_t>(scratch.size());
      buf = allocBuf(newSize);
      std::memcpy(buf->chunks(), scratch.data(),
                  static_cast<size_t>(newSize) * sizeof(Chunk));
      buf->size = newSize;
    }
    return true;
  }

  /// @brief Replace this set with (@p lhs \ @p rhs) in a single linear pass.
  /// Reuses capacity when uniquely owned; O(1) when lhs and rhs share a buffer.
  void setDifference(const AndersPtsSet &lhs, const AndersPtsSet &rhs) {
    if (lhs.buf == rhs.buf || lhs.isEmpty()) {
      clear();
      return;
    }
    if (rhs.isEmpty()) {
      *this = lhs;
      return;
    }
    if (this == &lhs || this == &rhs) {
      AndersPtsSet tmp;
      tmp.setDifference(lhs, rhs);
      *this = std::move(tmp);
      return;
    }

    const Chunk *a = lhs.buf->chunks();
    const Chunk *b = rhs.buf->chunks();
    const uint32_t n = lhs.buf->size, m = rhs.buf->size;

    static thread_local std::vector<Chunk> scratch;
    scratch.clear();
    scratch.reserve(n);
    uint32_t i = 0, j = 0;
    while (i < n && j < m) {
      if (a[i].idx < b[j].idx) {
        scratch.push_back(a[i++]);
      } else if (b[j].idx < a[i].idx) {
        ++j;
      } else {
        Chunk out;
        out.idx = a[i].idx;
        uint64_t any = 0;
        for (unsigned w = 0; w < kWordsPerChunk; ++w) {
          uint64_t v = a[i].words[w] & ~b[j].words[w];
          out.words[w] = v;
          any |= v;
        }
        if (any != 0)
          scratch.push_back(out);
        ++i;
        ++j;
      }
    }
    while (i < n)
      scratch.push_back(a[i++]);

    uint32_t newSize = static_cast<uint32_t>(scratch.size());
    if (newSize == 0) {
      clear();
      return;
    }
    if (buf && buf->refCount.load(std::memory_order_acquire) == 1u &&
        buf->cap >= newSize) {
      std::memcpy(buf->chunks(), scratch.data(),
                  static_cast<size_t>(newSize) * sizeof(Chunk));
      buf->size = newSize;
      return;
    }
    Buf *nb = allocBuf(newSize);
    std::memcpy(nb->chunks(), scratch.data(),
                static_cast<size_t>(newSize) * sizeof(Chunk));
    nb->size = newSize;
    release(buf);
    buf = nb;
  }

  /// @brief Remove all elements (preserving buffer capacity if uniquely owned).
  void clear() {
    if (!buf)
      return;
    if (buf->refCount.load(std::memory_order_acquire) == 1u) {
      buf->size = 0;
    } else {
      release(buf);
      buf = nullptr;
    }
  }

  /// @brief Remove all elements and release reference/heap storage.
  void clearAndRelease() {
    release(buf);
    buf = nullptr;
  }

  /// @brief Return the number of elements.
  unsigned getSize() const {
    if (!buf || buf->size == 0)
      return 0;
    unsigned total = 0;
    const Chunk *arr = buf->chunks();
    for (uint32_t i = 0, n = buf->size; i < n; ++i) {
      for (unsigned w = 0; w < kWordsPerChunk; ++w)
        total += popcount64(arr[i].words[w]);
    }
    return total;
  }

  /// @brief Return `true` if the set contains only the single element @p idx.
  bool containsOnly(unsigned idx) const {
    if (!buf || buf->size != 1u)
      return false;
    const Chunk &c = buf->chunks()[0];
    const uint32_t chunkIdx = idx / kChunkBits;
    if (c.idx != chunkIdx)
      return false;
    const unsigned off = idx % kChunkBits;
    const unsigned word = off >> 6;
    const uint64_t mask = uint64_t{1} << (off & 63u);
    for (unsigned w = 0; w < kWordsPerChunk; ++w) {
      if (c.words[w] != (w == word ? mask : 0))
        return false;
    }
    return true;
  }

  bool isEmpty() const { return !buf || buf->size == 0; }

  bool operator==(const AndersPtsSet &other) const {
    const bool e1 = isEmpty();
    const bool e2 = other.isEmpty();
    if (e1 || e2)
      return e1 == e2;
    if (buf == other.buf)
      return true;
    if (buf->size != other.buf->size)
      return false;
    return std::memcmp(buf->chunks(), other.buf->chunks(),
                       static_cast<size_t>(buf->size) * sizeof(Chunk)) == 0;
  }
  bool operator!=(const AndersPtsSet &other) const {
    return !(*this == other);
  }

  iterator begin() const {
    return isEmpty() ? iterator() : iterator(buf->chunks(), buf->size, false);
  }
  iterator end() const { return iterator(); }
};
