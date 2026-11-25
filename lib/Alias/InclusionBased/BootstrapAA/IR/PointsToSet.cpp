#include "Alias/InclusionBased/BootstrapAA/Engine.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace lotus {
namespace bootstrap {

namespace {
inline std::uint32_t popcount64(std::uint64_t x) noexcept {
  return static_cast<std::uint32_t>(__builtin_popcountll(x));
}

inline unsigned ctz64(std::uint64_t x) noexcept {
  return static_cast<unsigned>(__builtin_ctzll(x));
}
} // namespace

static_assert(sizeof(PointsToSet) == 32,
              "PointsToSet should fit in half a 64-byte cache line");

// --- Lifecycle --------------------------------------------------------------

void PointsToSet::releaseHeap() noexcept {
  if (isHeap()) {
    delete[] m_storage.heap.chunks;
    m_storage.heap.chunks = nullptr;
    m_storage.heap.capacity = 0;
    m_storage.heap.last_hit = 0;
    m_header &= ~FLAG_HEAP;
  }
}

void PointsToSet::setTop() noexcept {
  releaseHeap();
  m_header = FLAG_TOP;
  m_size = 1;
  m_storage.inl = {};
}

void PointsToSet::clear() noexcept {
  releaseHeap();
  m_header = 0;
  m_size = 0;
  m_storage.inl = {};
}

PointsToSet::PointsToSet(const PointsToSet &other)
    : m_header(other.m_header), m_size(other.m_size) {
  if (!other.isHeap()) {
    m_storage.inl = other.m_storage.inl;
  } else {
    const std::uint32_t cnt = other.count();
    m_storage.heap.chunks = new HeapChunk[cnt];
    std::memcpy(m_storage.heap.chunks, other.m_storage.heap.chunks,
                static_cast<std::size_t>(cnt) * sizeof(HeapChunk));
    m_storage.heap.capacity = cnt;
    m_storage.heap.last_hit = other.m_storage.heap.last_hit;
    m_storage.heap.pad = 0;
  }
}

PointsToSet::PointsToSet(PointsToSet &&other) noexcept
    : m_header(other.m_header), m_size(other.m_size) {
  if (!other.isHeap()) {
    m_storage.inl = other.m_storage.inl;
  } else {
    m_storage.heap = other.m_storage.heap;
  }
  other.m_header = 0;
  other.m_size = 0;
  other.m_storage.inl = {};
}

PointsToSet &PointsToSet::operator=(const PointsToSet &other) {
  if (this == &other)
    return *this;
  if (!other.isHeap()) {
    releaseHeap();
    m_header = other.m_header;
    m_size = other.m_size;
    m_storage.inl = other.m_storage.inl;
    return *this;
  }
  const std::uint32_t cnt = other.count();
  if (isHeap() && m_storage.heap.capacity >= cnt) {
    std::memcpy(m_storage.heap.chunks, other.m_storage.heap.chunks,
                static_cast<std::size_t>(cnt) * sizeof(HeapChunk));
    m_header = other.m_header;
    m_size = other.m_size;
    m_storage.heap.last_hit = other.m_storage.heap.last_hit;
    return *this;
  }
  auto *fresh = new HeapChunk[cnt];
  std::memcpy(fresh, other.m_storage.heap.chunks,
              static_cast<std::size_t>(cnt) * sizeof(HeapChunk));
  releaseHeap();
  m_header = other.m_header;
  m_size = other.m_size;
  m_storage.heap.chunks = fresh;
  m_storage.heap.capacity = cnt;
  m_storage.heap.last_hit = other.m_storage.heap.last_hit;
  m_storage.heap.pad = 0;
  return *this;
}

PointsToSet &PointsToSet::operator=(PointsToSet &&other) noexcept {
  if (this == &other)
    return *this;
  releaseHeap();
  m_header = other.m_header;
  m_size = other.m_size;
  if (!other.isHeap()) {
    m_storage.inl = other.m_storage.inl;
  } else {
    m_storage.heap = other.m_storage.heap;
  }
  other.m_header = 0;
  other.m_size = 0;
  other.m_storage.inl = {};
  return *this;
}

PointsToSet::~PointsToSet() {
  if (isHeap())
    delete[] m_storage.heap.chunks;
}

// --- Iterator ---------------------------------------------------------------

PointsToSet::const_iterator::const_iterator(const PointsToSet *set,
                                            std::uint32_t pos,
                                            std::uint64_t remaining)
    : m_set(set), m_pos(pos), m_remaining(remaining) {}

Id PointsToSet::const_iterator::operator*() const {
  if (m_pos == POS_TOP)
    return UNKNOWN;
  if (m_pos == POS_NULL)
    return NULL_OBJECT;
  const unsigned bit = ctz64(m_remaining);
  if (!m_set->isHeap())
    return static_cast<Id>(m_set->m_storage.inl.idx[m_pos] * WORD_BITS + bit);
  const std::uint32_t ci = m_pos >> 2;
  const std::uint32_t sub = m_pos & 3u;
  return static_cast<Id>(
      (m_set->m_storage.heap.chunks[ci].index * CHUNK_WORDS + sub) * WORD_BITS +
      bit);
}

PointsToSet::const_iterator &PointsToSet::const_iterator::operator++() {
  if (m_pos == POS_TOP) {
    m_pos = POS_END;
    m_remaining = 0;
    return *this;
  }
  if (m_pos == POS_NULL) {
    const std::uint32_t cnt = m_set->count();
    if (cnt == 0) {
      m_pos = POS_END;
      m_remaining = 0;
      return *this;
    }
    if (!m_set->isHeap()) {
      m_pos = 0;
      m_remaining = m_set->m_storage.inl.bits[0];
      return *this;
    }
    const HeapChunk &c = m_set->m_storage.heap.chunks[0];
    for (std::uint32_t s = 0; s < CHUNK_WORDS; ++s) {
      if (c.words[s] != 0) {
        m_pos = s;
        m_remaining = c.words[s];
        return *this;
      }
    }
    m_pos = POS_END;
    m_remaining = 0;
    return *this;
  }

  m_remaining &= m_remaining - 1;
  if (m_remaining != 0)
    return *this;

  const std::uint32_t cnt = m_set->count();
  if (!m_set->isHeap()) {
    ++m_pos;
    if (m_pos < cnt) {
      m_remaining = m_set->m_storage.inl.bits[m_pos];
    } else {
      m_pos = POS_END;
      m_remaining = 0;
    }
    return *this;
  }

  std::uint32_t ci = m_pos >> 2;
  std::uint32_t sub = (m_pos & 3u) + 1u;
  while (ci < cnt) {
    const HeapChunk &c = m_set->m_storage.heap.chunks[ci];
    while (sub < CHUNK_WORDS) {
      if (c.words[sub] != 0) {
        m_pos = (ci << 2) | sub;
        m_remaining = c.words[sub];
        return *this;
      }
      ++sub;
    }
    ++ci;
    sub = 0;
  }
  m_pos = POS_END;
  m_remaining = 0;
  return *this;
}

bool PointsToSet::const_iterator::operator==(
    const const_iterator &other) const {
  return m_set == other.m_set && m_pos == other.m_pos &&
         m_remaining == other.m_remaining;
}

PointsToSet::const_iterator PointsToSet::ObjectRange::begin() const {
  return m_set->begin();
}

PointsToSet::const_iterator PointsToSet::ObjectRange::end() const {
  return m_set->end();
}

PointsToSet::const_iterator PointsToSet::begin() const {
  if (empty())
    return end();
  if (isTop())
    return const_iterator(this, const_iterator::POS_TOP, 0);
  if (hasNull())
    return const_iterator(this, const_iterator::POS_NULL, 0);
  if (!isHeap())
    return const_iterator(this, 0, m_storage.inl.bits[0]);
  const HeapChunk &c = m_storage.heap.chunks[0];
  for (std::uint32_t s = 0; s < CHUNK_WORDS; ++s) {
    if (c.words[s] != 0)
      return const_iterator(this, s, c.words[s]);
  }
  return end();
}

PointsToSet::const_iterator PointsToSet::end() const {
  return const_iterator(this, const_iterator::POS_END, 0);
}

// --- Heap Chunk Helpers -----------------------------------------------------

std::uint32_t PointsToSet::heapFindChunk(Id chunk_idx) const noexcept {
  const std::uint32_t cnt = count();
  if (cnt == 0)
    return 0;
  const HeapChunk *chunks = m_storage.heap.chunks;
  const std::uint32_t hint = m_storage.heap.last_hit;
  if (hint < cnt && chunks[hint].index == chunk_idx)
    return hint;
  if (chunks[cnt - 1].index < chunk_idx)
    return cnt;
  if (chunks[cnt - 1].index == chunk_idx) {
    m_storage.heap.last_hit = cnt - 1;
    return cnt - 1;
  }
  std::uint32_t lo = 0;
  std::uint32_t hi = cnt - 1;
  while (lo < hi) {
    const std::uint32_t mid = lo + ((hi - lo) >> 1);
    if (chunks[mid].index < chunk_idx)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (chunks[lo].index == chunk_idx)
    m_storage.heap.last_hit = lo;
  return lo;
}

void PointsToSet::heapGrow(std::uint32_t min_cap) {
  std::uint32_t new_cap =
      m_storage.heap.capacity == 0 ? 4u : m_storage.heap.capacity * 2u;
  if (new_cap < min_cap)
    new_cap = min_cap;
  auto *fresh = new HeapChunk[new_cap];
  const std::uint32_t cnt = count();
  if (cnt > 0)
    std::memcpy(fresh, m_storage.heap.chunks,
                static_cast<std::size_t>(cnt) * sizeof(HeapChunk));
  delete[] m_storage.heap.chunks;
  m_storage.heap.chunks = fresh;
  m_storage.heap.capacity = new_cap;
}

bool PointsToSet::heapInsertWord(Id word_idx, std::uint64_t mask,
                                 bool update_size) {
  const Id chunk_idx = word_idx >> 2;
  const unsigned sub = static_cast<unsigned>(word_idx & 3u);
  const std::uint32_t cnt = count();
  const std::uint32_t pos = heapFindChunk(chunk_idx);
  if (pos < cnt && m_storage.heap.chunks[pos].index == chunk_idx) {
    std::uint64_t &w = m_storage.heap.chunks[pos].words[sub];
    const std::uint64_t added = mask & ~w;
    if (!added)
      return false;
    w |= added;
    if (update_size)
      m_size += popcount64(added);
    return true;
  }
  if (cnt == m_storage.heap.capacity)
    heapGrow(cnt + 1);
  if (pos < cnt)
    std::memmove(m_storage.heap.chunks + pos + 1, m_storage.heap.chunks + pos,
                 static_cast<std::size_t>(cnt - pos) * sizeof(HeapChunk));
  HeapChunk &c = m_storage.heap.chunks[pos];
  c = HeapChunk{};
  c.index = chunk_idx;
  c.words[sub] = mask;
  setCount(cnt + 1);
  m_storage.heap.last_hit = pos;
  if (update_size)
    m_size += popcount64(mask);
  return true;
}

void PointsToSet::promoteInlineToHeap(Id extra_word_idx,
                                      std::uint64_t extra_mask) {
  const InlineStorage saved = m_storage.inl;
  const bool had_null = hasNull();
  auto *buf = new HeapChunk[4];
  m_header = (had_null ? FLAG_NULL : 0u) | FLAG_HEAP;
  m_storage.heap.chunks = buf;
  m_storage.heap.capacity = 4;
  m_storage.heap.last_hit = 0;
  m_storage.heap.pad = 0;
  heapInsertWord(saved.idx[0], saved.bits[0], /*update_size=*/false);
  heapInsertWord(saved.idx[1], saved.bits[1], /*update_size=*/false);
  heapInsertWord(extra_word_idx, extra_mask, /*update_size=*/true);
}

bool PointsToSet::insertWord(Id word_idx, std::uint64_t mask) {
  if (!isHeap()) {
    const std::uint32_t cnt = count();
    if (cnt == 0) {
      m_storage.inl.idx[0] = word_idx;
      m_storage.inl.bits[0] = mask;
      setCount(1);
      m_size += popcount64(mask);
      return true;
    }
    if (m_storage.inl.idx[0] == word_idx) {
      const std::uint64_t added = mask & ~m_storage.inl.bits[0];
      if (!added)
        return false;
      m_storage.inl.bits[0] |= added;
      m_size += popcount64(added);
      return true;
    }
    if (cnt == 1) {
      if (word_idx > m_storage.inl.idx[0]) {
        m_storage.inl.idx[1] = word_idx;
        m_storage.inl.bits[1] = mask;
      } else {
        m_storage.inl.idx[1] = m_storage.inl.idx[0];
        m_storage.inl.bits[1] = m_storage.inl.bits[0];
        m_storage.inl.idx[0] = word_idx;
        m_storage.inl.bits[0] = mask;
      }
      setCount(2);
      m_size += popcount64(mask);
      return true;
    }
    // cnt == 2
    if (m_storage.inl.idx[1] == word_idx) {
      const std::uint64_t added = mask & ~m_storage.inl.bits[1];
      if (!added)
        return false;
      m_storage.inl.bits[1] |= added;
      m_size += popcount64(added);
      return true;
    }
    promoteInlineToHeap(word_idx, mask);
    return true;
  }
  return heapInsertWord(word_idx, mask, /*update_size=*/true);
}

// --- Core Set Operations ----------------------------------------------------

bool PointsToSet::contains(Id object) const {
  if (isTop())
    return true;
  if (object == INVALID)
    return false;
  if (object == UNKNOWN)
    return false;
  if (object == NULL_OBJECT)
    return hasNull();
  const std::uint32_t cnt = count();
  if (cnt == 0)
    return false;
  const Id word_idx = object / WORD_BITS;
  const std::uint64_t mask = std::uint64_t{1} << (object % WORD_BITS);
  if (!isHeap()) {
    if (m_storage.inl.idx[0] == word_idx)
      return (m_storage.inl.bits[0] & mask) != 0;
    if (cnt == 2 && m_storage.inl.idx[1] == word_idx)
      return (m_storage.inl.bits[1] & mask) != 0;
    return false;
  }
  const Id chunk_idx = word_idx >> 2;
  const unsigned sub = static_cast<unsigned>(word_idx & 3u);
  const std::uint32_t pos = heapFindChunk(chunk_idx);
  return pos < cnt && m_storage.heap.chunks[pos].index == chunk_idx &&
         (m_storage.heap.chunks[pos].words[sub] & mask) != 0;
}

bool PointsToSet::insert(Id object) {
  if (object == INVALID)
    throw std::invalid_argument("BootstrapAA: invalid points-to object ID");
  if (isTop())
    return false;
  if (object == UNKNOWN) {
    setTop();
    return true;
  }
  if (object == NULL_OBJECT) {
    if (hasNull())
      return false;
    m_header |= FLAG_NULL;
    ++m_size;
    return true;
  }
  return insertWord(object / WORD_BITS,
                    std::uint64_t{1} << (object % WORD_BITS));
}

bool PointsToSet::join(const PointsToSet &other) {
  if (this == &other || isTop() || other.empty())
    return false;
  if (other.isTop()) {
    setTop();
    return true;
  }
  bool changed = false;
  if (other.hasNull() && !hasNull()) {
    m_header |= FLAG_NULL;
    ++m_size;
    changed = true;
  }
  const std::uint32_t ocnt = other.count();
  if (ocnt == 0)
    return changed;

  // Fast path: other is inline (1 or 2 64-bit words).
  if (!other.isHeap()) {
    for (std::uint32_t k = 0; k < ocnt; ++k)
      changed |=
          insertWord(other.m_storage.inl.idx[k], other.m_storage.inl.bits[k]);
    return changed;
  }

  // other is Heap (>= 3 distinct non-zero 64-bit words).
  if (!isHeap()) {
    const std::uint32_t saved_cnt = count();
    const InlineStorage saved_inl = m_storage.inl;
    const bool had_null = hasNull();
    const std::uint32_t new_cap = std::max<std::uint32_t>(ocnt + saved_cnt, 4u);
    auto *buf = new HeapChunk[new_cap];
    std::memcpy(buf, other.m_storage.heap.chunks,
                static_cast<std::size_t>(ocnt) * sizeof(HeapChunk));
    m_header = (had_null ? FLAG_NULL : 0u) | FLAG_HEAP | ocnt;
    m_size = (had_null ? 1u : 0u) +
             (other.m_size - (other.hasNull() ? 1u : 0u));
    m_storage.heap.chunks = buf;
    m_storage.heap.capacity = new_cap;
    m_storage.heap.last_hit = other.m_storage.heap.last_hit;
    m_storage.heap.pad = 0;
    for (std::uint32_t k = 0; k < saved_cnt; ++k)
      heapInsertWord(saved_inl.idx[k], saved_inl.bits[k], /*update_size=*/true);
    // Because `this` was inline (<= 2 non-zero 64-bit words) and `other` is
    // heap (>= 3 non-zero 64-bit words), `other` strictly added elements.
    return true;
  }

  // Both are Heap: Pass 1 performs in-place OR on all matching chunks with
  // zero allocations, and counts any missing chunks from `other`.
  const std::uint32_t n1 = count();
  const std::uint32_t n2 = ocnt;
  HeapChunk *c1 = m_storage.heap.chunks;
  const HeapChunk *c2 = other.m_storage.heap.chunks;
  std::uint32_t i = 0;
  std::uint32_t j = 0;
  std::uint32_t missing = 0;
  while (i < n1 && j < n2) {
    if (c1[i].index < c2[j].index) {
      ++i;
    } else if (c1[i].index > c2[j].index) {
      ++missing;
      ++j;
    } else {
      for (unsigned w = 0; w < CHUNK_WORDS; ++w) {
        const std::uint64_t added = c2[j].words[w] & ~c1[i].words[w];
        if (added) {
          c1[i].words[w] |= added;
          m_size += popcount64(added);
          changed = true;
        }
      }
      ++i;
      ++j;
    }
  }
  missing += (n2 - j);
  if (missing == 0)
    return changed;

  // Pass 2: insert the `missing` chunks (in-place backwards if capacity
  // suffices, otherwise one geometric allocation).
  const std::uint32_t new_cnt = n1 + missing;
  auto chunkPopcount = [](const HeapChunk &c) noexcept {
    return popcount64(c.words[0]) + popcount64(c.words[1]) +
           popcount64(c.words[2]) + popcount64(c.words[3]);
  };
  if (new_cnt <= m_storage.heap.capacity) {
    std::uint32_t dst = new_cnt;
    std::uint32_t p1 = n1;
    std::uint32_t p2 = n2;
    while (p2 > 0) {
      if (p1 == 0 || c2[p2 - 1].index > c1[p1 - 1].index) {
        --p2;
        c1[--dst] = c2[p2];
        m_size += chunkPopcount(c2[p2]);
      } else if (c1[p1 - 1].index > c2[p2 - 1].index) {
        --p1;
        c1[--dst] = c1[p1];
      } else {
        --p1;
        --p2;
        c1[--dst] = c1[p1];
      }
    }
    setCount(new_cnt);
    return true;
  }

  std::uint32_t new_cap =
      std::max<std::uint32_t>(new_cnt, m_storage.heap.capacity * 2u);
  auto *fresh = new HeapChunk[new_cap];
  std::uint32_t dst = 0;
  std::uint32_t p1 = 0;
  std::uint32_t p2 = 0;
  while (p1 < n1 && p2 < n2) {
    if (c1[p1].index < c2[p2].index) {
      fresh[dst++] = c1[p1++];
    } else if (c2[p2].index < c1[p1].index) {
      m_size += chunkPopcount(c2[p2]);
      fresh[dst++] = c2[p2++];
    } else {
      fresh[dst++] = c1[p1++];
      ++p2;
    }
  }
  while (p1 < n1)
    fresh[dst++] = c1[p1++];
  while (p2 < n2) {
    m_size += chunkPopcount(c2[p2]);
    fresh[dst++] = c2[p2++];
  }
  delete[] m_storage.heap.chunks;
  m_storage.heap.chunks = fresh;
  m_storage.heap.capacity = new_cap;
  m_storage.heap.last_hit = 0;
  setCount(new_cnt);
  return true;
}

bool PointsToSet::intersectsNonNull(const PointsToSet &other) const {
  if (!hasNonNull() || !other.hasNonNull())
    return false;
  if (isTop() || other.isTop())
    return true;

  const std::uint32_t n1 = count();
  const std::uint32_t n2 = other.count();
  if (!isHeap() && !other.isHeap()) {
    for (std::uint32_t i = 0; i < n1; ++i) {
      for (std::uint32_t j = 0; j < n2; ++j) {
        if (m_storage.inl.idx[i] == other.m_storage.inl.idx[j] &&
            (m_storage.inl.bits[i] & other.m_storage.inl.bits[j]) != 0)
          return true;
      }
    }
    return false;
  }
  if (!isHeap() && other.isHeap()) {
    for (std::uint32_t i = 0; i < n1; ++i) {
      const Id wi = m_storage.inl.idx[i];
      const Id ci = wi >> 2;
      const unsigned sub = static_cast<unsigned>(wi & 3u);
      const std::uint32_t pos = other.heapFindChunk(ci);
      if (pos < n2 && other.m_storage.heap.chunks[pos].index == ci &&
          (other.m_storage.heap.chunks[pos].words[sub] &
           m_storage.inl.bits[i]) != 0)
        return true;
    }
    return false;
  }
  if (isHeap() && !other.isHeap())
    return other.intersectsNonNull(*this);

  const HeapChunk *c1 = m_storage.heap.chunks;
  const HeapChunk *c2 = other.m_storage.heap.chunks;
  std::uint32_t i = 0;
  std::uint32_t j = 0;
  while (i < n1 && j < n2) {
    if (c1[i].index < c2[j].index) {
      ++i;
    } else if (c2[j].index < c1[i].index) {
      ++j;
    } else {
      const std::uint64_t any = (c1[i].words[0] & c2[j].words[0]) |
                                (c1[i].words[1] & c2[j].words[1]) |
                                (c1[i].words[2] & c2[j].words[2]) |
                                (c1[i].words[3] & c2[j].words[3]);
      if (any != 0)
        return true;
      ++i;
      ++j;
    }
  }
  return false;
}

bool PointsToSet::intersects(const PointsToSet &other) const {
  if (empty() || other.empty())
    return false;
  if (isTop() || other.isTop())
    return true;
  if (hasNull() && other.hasNull())
    return true;
  return intersectsNonNull(other);
}

bool PointsToSet::operator==(const PointsToSet &rhs) const {
  if (m_header != rhs.m_header || m_size != rhs.m_size)
    return false;
  const std::uint32_t cnt = count();
  if (isTop() || cnt == 0)
    return true;
  if (!isHeap()) {
    for (std::uint32_t k = 0; k < cnt; ++k) {
      if (m_storage.inl.idx[k] != rhs.m_storage.inl.idx[k] ||
          m_storage.inl.bits[k] != rhs.m_storage.inl.bits[k])
        return false;
    }
    return true;
  }
  const HeapChunk *a = m_storage.heap.chunks;
  const HeapChunk *b = rhs.m_storage.heap.chunks;
  for (std::uint32_t k = 0; k < cnt; ++k) {
    if (a[k].index != b[k].index || a[k].words[0] != b[k].words[0] ||
        a[k].words[1] != b[k].words[1] || a[k].words[2] != b[k].words[2] ||
        a[k].words[3] != b[k].words[3])
      return false;
  }
  return true;
}

bool PointsToSet::operator<(const PointsToSet &rhs) const {
  if (m_header != rhs.m_header)
    return m_header < rhs.m_header;
  const std::uint32_t cnt = count();
  if (isTop() || cnt == 0)
    return false;
  if (!isHeap()) {
    for (std::uint32_t k = 0; k < cnt; ++k) {
      if (m_storage.inl.idx[k] != rhs.m_storage.inl.idx[k])
        return m_storage.inl.idx[k] < rhs.m_storage.inl.idx[k];
      if (m_storage.inl.bits[k] != rhs.m_storage.inl.bits[k])
        return m_storage.inl.bits[k] < rhs.m_storage.inl.bits[k];
    }
    return false;
  }
  const HeapChunk *a = m_storage.heap.chunks;
  const HeapChunk *b = rhs.m_storage.heap.chunks;
  for (std::uint32_t k = 0; k < cnt; ++k) {
    if (a[k].index != b[k].index)
      return a[k].index < b[k].index;
    for (unsigned w = 0; w < CHUNK_WORDS; ++w) {
      if (a[k].words[w] != b[k].words[w])
        return a[k].words[w] < b[k].words[w];
    }
  }
  return false;
}

bool SteensgaardHierarchy::isHigher(Id higher, Id lower) const {
  if (higher >= successors.size() || lower >= successors.size() ||
      higher == lower)
    return false;
  std::vector<bool> visited(successors.size(), false);
  std::vector<Id> work{higher};
  while (!work.empty()) {
    const Id current = work.back();
    work.pop_back();
    if (!visited[current]) {
      visited[current] = true;
      for (Id successor : successors[current]) {
        if (successor == lower)
          return true;
        work.push_back(successor);
      }
    }
  }
  return false;
}

} // namespace bootstrap
} // namespace lotus
