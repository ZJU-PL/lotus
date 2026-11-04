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
// IDs are stored in a sparse bitset: a sorted vector of 64-bit words, each
// covering 64 consecutive object IDs. Object IDs from the SVFG builder are
// dense, so a set usually occupies one or two words.
//
// The API is the subset of std::set used by Lotus (insert/count/find/erase/
// iteration/==/<), including the `insert(iterator, value)` form required by
// std::inserter. Iteration is in increasing order, so std::set_intersection
// and similar algorithms behave as before.
//
// Clustering is not implemented here; the VFS solver keeps its equivalence
// map in equivalentObject_.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
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
  /// A dense 64-bit word covering IDs [base * 64, base * 64 + 63].
  /// `chunks_` is sorted by `base` and never contains an empty word.
  struct Word {
    std::uint32_t base = 0;
    std::uint64_t bits = 0;

    friend bool operator==(const Word &lhs, const Word &rhs) {
      return lhs.base == rhs.base && lhs.bits == rhs.bits;
    }
    friend bool operator<(const Word &lhs, const Word &rhs) {
      return lhs.base < rhs.base;
    }
  };

  std::vector<Word> chunks_;
  size_type size_ = 0;

  static constexpr std::uint32_t wordIndex(std::uint32_t id) { return id >> 6; }
  static constexpr std::uint64_t bitMask(std::uint32_t id) {
    return std::uint64_t(1) << (id & 63);
  }

  size_type lowerBoundIndex(std::uint32_t index) const {
    return static_cast<size_type>(
        std::lower_bound(chunks_.begin(), chunks_.end(), index,
                         [](const Word &word, std::uint32_t value) {
                           return word.base < value;
                         }) -
        chunks_.begin());
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
    std::uint64_t pending_ = 0; // unconsumed bits of chunks_[chunk_]
    std::uint32_t current_ = 0;

    void advance() {
      if (!owner_)
        return;
      while (pending_ == 0) {
        ++chunk_;
        if (chunk_ >= owner_->chunks_.size()) {
          current_ = 0;
          return;
        }
        pending_ = owner_->chunks_[chunk_].bits;
      }
      const unsigned bit = static_cast<unsigned>(__builtin_ctzll(pending_));
      current_ = (owner_->chunks_[chunk_].base << 6) | bit;
      pending_ &= pending_ - 1; // clear the lowest set bit
    }

  public:
    const_iterator() = default;

    const_iterator(const SVFGNodeBS *owner, size_type chunk,
                   std::uint64_t pending, bool doAdvance = true)
        : owner_(owner), chunk_(chunk), pending_(pending) {
      if (doAdvance)
        advance();
    }

    std::uint32_t operator*() const { return current_; }
    pointer operator->() const { return &current_; }

    const_iterator &operator++() {
      advance();
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator tmp = *this;
      advance();
      return tmp;
    }

    friend bool operator==(const const_iterator &lhs,
                           const const_iterator &rhs) {
      return lhs.owner_ == rhs.owner_ && lhs.chunk_ == rhs.chunk_ &&
             lhs.current_ == rhs.current_;
    }
    friend bool operator!=(const const_iterator &lhs,
                           const const_iterator &rhs) {
      return !(lhs == rhs);
    }
  };

  using iterator = const_iterator;

  SVFGNodeBS() = default;

  SVFGNodeBS(std::initializer_list<std::uint32_t> values) {
    for (std::uint32_t value : values)
      insert(value);
  }

  /// Converting constructor for call sites that build a std::set<uint32_t>.
  SVFGNodeBS(const std::set<std::uint32_t> &values) {
    for (std::uint32_t value : values)
      insert(value);
  }

  //===------------------------------------------------------------------===
  // Capacity
  //===------------------------------------------------------------------===

  bool empty() const { return size_ == 0; }
  size_type size() const { return size_; }

  //===------------------------------------------------------------------===
  // Iteration (ascending object ID order)
  //===------------------------------------------------------------------===

  const_iterator begin() const {
    if (chunks_.empty())
      return end();
    return const_iterator(this, 0, chunks_.front().bits);
  }
  const_iterator end() const {
    return const_iterator(this, chunks_.size(), 0, false);
  }

  //===------------------------------------------------------------------===
  // Lookup
  //===------------------------------------------------------------------===

  bool contains(std::uint32_t id) const { return count(id) != 0; }

  size_type count(std::uint32_t id) const {
    const size_type index = lowerBoundIndex(wordIndex(id));
    if (index == chunks_.size() || chunks_[index].base != wordIndex(id))
      return 0;
    return (chunks_[index].bits & bitMask(id)) != 0 ? 1 : 0;
  }

  const_iterator find(std::uint32_t id) const {
    const size_type index = lowerBoundIndex(wordIndex(id));
    if (index == chunks_.size() || chunks_[index].base != wordIndex(id))
      return end();
    const std::uint64_t bits = chunks_[index].bits;
    if ((bits & bitMask(id)) == 0)
      return end();
    // Keep only the bits at or above `id` so the lowest set bit is `id`.
    return const_iterator(this, index, bits & (~std::uint64_t(0) << (id & 63)));
  }

  //===------------------------------------------------------------------===
  // Modifiers
  //===------------------------------------------------------------------===

  std::pair<iterator, bool> insert(std::uint32_t id) {
    const size_type index = lowerBoundIndex(wordIndex(id));
    if (index != chunks_.size() && chunks_[index].base == wordIndex(id)) {
      if ((chunks_[index].bits & bitMask(id)) != 0)
        return {find(id), false};
      chunks_[index].bits |= bitMask(id);
      ++size_;
      return {find(id), true};
    }
    chunks_.insert(chunks_.begin() + index, Word{wordIndex(id), bitMask(id)});
    ++size_;
    return {find(id), true};
  }

  /// Union sorted words in linear time instead of inserting individual bits.
  bool unite(const SVFGNodeBS &other) {
    if (other.empty() || *this == other)
      return false;
    if (empty()) {
      *this = other;
      return true;
    }
    // Tiny updates are cheaper in place and avoid allocating a merge buffer.
    if (other.size_ <= 4) {
      const size_type oldSize = size_;
      insert(other.begin(), other.end());
      return size_ != oldSize;
    }
    std::vector<Word> merged;
    merged.reserve(chunks_.size() + other.chunks_.size());
    size_type left = 0, right = 0, count = 0;
    while (left < chunks_.size() || right < other.chunks_.size()) {
      Word word;
      if (right == other.chunks_.size() ||
          (left < chunks_.size() &&
           chunks_[left].base < other.chunks_[right].base)) {
        word = chunks_[left++];
      } else if (left == chunks_.size() ||
                 other.chunks_[right].base < chunks_[left].base) {
        word = other.chunks_[right++];
      } else {
        word = {chunks_[left].base,
                chunks_[left].bits | other.chunks_[right].bits};
        ++left;
        ++right;
      }
      count += __builtin_popcountll(word.bits);
      merged.push_back(word);
    }
    if (count == size_)
      return false;
    chunks_ = std::move(merged);
    size_ = count;
    return true;
  }

  /// Remove a set in word-linear time, retaining sorted nonempty chunks.
  bool subtract(const SVFGNodeBS &other) {
    if (this == &other) {
      const bool changed = !empty();
      clear();
      return changed;
    }
    size_type right = 0, output = 0, removed = 0;
    for (size_type left = 0; left < chunks_.size(); ++left) {
      Word word = chunks_[left];
      while (right < other.chunks_.size() &&
             other.chunks_[right].base < word.base)
        ++right;
      if (right < other.chunks_.size() &&
          other.chunks_[right].base == word.base) {
        removed += __builtin_popcountll(word.bits & other.chunks_[right].bits);
        word.bits &= ~other.chunks_[right].bits;
      }
      if (word.bits)
        chunks_[output++] = word;
    }
    chunks_.resize(output);
    size_ -= removed;
    return removed != 0;
  }

  /// Hint overload required by std::insert_iterator (std::inserter).
  iterator insert(iterator, std::uint32_t id) { return insert(id).first; }

  template <typename InputIt> void insert(InputIt first, InputIt last) {
    for (; first != last; ++first)
      insert(static_cast<std::uint32_t>(*first));
  }

  void insert(std::initializer_list<std::uint32_t> values) {
    for (std::uint32_t value : values)
      insert(value);
  }

  size_type erase(std::uint32_t id) {
    const size_type index = lowerBoundIndex(wordIndex(id));
    if (index == chunks_.size() || chunks_[index].base != wordIndex(id))
      return 0;
    if ((chunks_[index].bits & bitMask(id)) == 0)
      return 0;
    chunks_[index].bits &= ~bitMask(id);
    if (chunks_[index].bits == 0)
      chunks_.erase(chunks_.begin() + index);
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

  void clear() {
    chunks_.clear();
    size_ = 0;
  }

  void swap(SVFGNodeBS &other) noexcept {
    chunks_.swap(other.chunks_);
    std::swap(size_, other.size_);
  }

  //===------------------------------------------------------------------===
  // Comparison
  //===------------------------------------------------------------------===

  friend bool operator==(const SVFGNodeBS &lhs, const SVFGNodeBS &rhs) {
    return lhs.size_ == rhs.size_ && lhs.chunks_ == rhs.chunks_;
  }
  friend bool operator!=(const SVFGNodeBS &lhs, const SVFGNodeBS &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const SVFGNodeBS &lhs, const SVFGNodeBS &rhs) {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                        rhs.end());
  }
};

} // namespace analysis
} // namespace lotus
