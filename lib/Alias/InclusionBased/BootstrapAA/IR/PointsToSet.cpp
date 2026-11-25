#include "Alias/InclusionBased/BootstrapAA/Engine.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace lotus {
namespace bootstrap {

PointsToSet::const_iterator::const_iterator(const PointsToSet *set,
                                            std::size_t word,
                                            std::uint64_t remaining)
    : m_set(set), m_word(word), m_remaining(remaining) {}

Id PointsToSet::const_iterator::operator*() const {
  const auto bit = static_cast<unsigned>(__builtin_ctzll(m_remaining));
  return static_cast<Id>(m_set->m_words[m_word].index * WORD_BITS + bit);
}

PointsToSet::const_iterator &PointsToSet::const_iterator::operator++() {
  m_remaining &= m_remaining - 1;
  if (m_remaining != 0)
    return *this;
  ++m_word;
  if (m_word < m_set->m_words.size())
    m_remaining = m_set->m_words[m_word].bits;
  return *this;
}

bool PointsToSet::const_iterator::operator==(
    const const_iterator &other) const {
  return m_set == other.m_set && m_word == other.m_word &&
         m_remaining == other.m_remaining;
}

PointsToSet::const_iterator PointsToSet::ObjectRange::begin() const {
  return m_set->begin();
}

PointsToSet::const_iterator PointsToSet::ObjectRange::end() const {
  return m_set->end();
}

PointsToSet::const_iterator PointsToSet::begin() const {
  return empty() ? end() : const_iterator(this, 0, m_words.front().bits);
}

PointsToSet::const_iterator PointsToSet::end() const {
  return const_iterator(this, m_words.size(), 0);
}

bool PointsToSet::isTop() const {
  return m_words.size() == 1 && m_words.front().index == 0 &&
         m_words.front().bits == 1;
}

std::size_t PointsToSet::size() const {
  std::size_t result = 0;
  for (const Word &word : m_words)
    result += static_cast<std::size_t>(__builtin_popcountll(word.bits));
  return result;
}

bool PointsToSet::contains(Id object) const {
  if (isTop())
    return true;
  if (object == INVALID)
    return false;
  const Id wordIndex = object / WORD_BITS;
  const auto found = std::lower_bound(
      m_words.begin(), m_words.end(), wordIndex,
      [](const Word &word, Id index) { return word.index < index; });
  return found != m_words.end() && found->index == wordIndex &&
         (found->bits & (std::uint64_t{1} << (object % WORD_BITS))) != 0;
}

bool PointsToSet::insert(Id object) {
  if (object == INVALID)
    throw std::invalid_argument("BootstrapAA: invalid points-to object ID");
  if (isTop() || contains(object))
    return false;
  if (object == UNKNOWN)
    m_words.clear();
  const Id wordIndex = object / WORD_BITS;
  auto found = std::lower_bound(
      m_words.begin(), m_words.end(), wordIndex,
      [](const Word &word, Id index) { return word.index < index; });
  if (found == m_words.end() || found->index != wordIndex)
    found = m_words.insert(found, Word{wordIndex, 0});
  found->bits |= std::uint64_t{1} << (object % WORD_BITS);
  return true;
}

bool PointsToSet::join(const PointsToSet &other) {
  if (isTop() || other.empty())
    return false;
  if (other.isTop()) {
    m_words = {{0, 1}};
    return true;
  }
  std::vector<Word> merged;
  merged.reserve(m_words.size() + other.m_words.size());
  bool changed = false;
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < m_words.size() || right < other.m_words.size()) {
    if (right == other.m_words.size() ||
        (left < m_words.size() &&
         m_words[left].index < other.m_words[right].index)) {
      merged.push_back(m_words[left++]);
    } else if (left == m_words.size() ||
               other.m_words[right].index < m_words[left].index) {
      merged.push_back(other.m_words[right++]);
      changed = true;
    } else {
      const std::uint64_t bits = m_words[left].bits | other.m_words[right].bits;
      changed |= bits != m_words[left].bits;
      merged.push_back({m_words[left].index, bits});
      ++left;
      ++right;
    }
  }
  if (changed)
    m_words = std::move(merged);
  return changed;
}

bool PointsToSet::intersects(const PointsToSet &other) const {
  if (empty() || other.empty())
    return false;
  if (isTop() || other.isTop())
    return true;
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < m_words.size() && right < other.m_words.size()) {
    if (m_words[left].index < other.m_words[right].index) {
      ++left;
    } else if (other.m_words[right].index < m_words[left].index) {
      ++right;
    } else {
      if ((m_words[left].bits & other.m_words[right].bits) != 0)
        return true;
      ++left;
      ++right;
    }
  }
  return false;
}

bool PointsToSet::operator==(const PointsToSet &rhs) const {
  return m_words == rhs.m_words;
}

bool PointsToSet::operator<(const PointsToSet &rhs) const {
  auto left = begin();
  auto right = rhs.begin();
  while (left != end() && right != rhs.end()) {
    if (*left != *right)
      return *left < *right;
    ++left;
    ++right;
  }
  return left == end() && right != rhs.end();
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
