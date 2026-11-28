#pragma once

#include "Solvers/EGraph/FlatHashMap.h"
#include "Solvers/EGraph/Id.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::egraph {

class Symbol {
public:
  Symbol() = default;
  Symbol(const char *text)
      : id_(intern(text ? std::string_view(text) : std::string_view())) {}
  Symbol(std::string text) : id_(intern(text)) {}
  Symbol(std::string_view text) : id_(intern(text)) {}

  uint32_t id() const { return id_; }

  const std::string &str() const { return table().strings.at(id_); }
  const char *c_str() const { return str().c_str(); }
  std::string_view view() const { return str(); }

  operator std::string() const { return str(); }
  operator std::string_view() const { return view(); }

  friend bool operator==(Symbol lhs, Symbol rhs) { return lhs.id_ == rhs.id_; }
  friend bool operator!=(Symbol lhs, Symbol rhs) { return !(lhs == rhs); }
  friend bool operator<(Symbol lhs, Symbol rhs) { return lhs.id_ < rhs.id_; }
  friend bool operator<=(Symbol lhs, Symbol rhs) { return !(rhs < lhs); }
  friend bool operator>(Symbol lhs, Symbol rhs) { return rhs < lhs; }
  friend bool operator>=(Symbol lhs, Symbol rhs) { return !(lhs < rhs); }

  friend bool operator==(Symbol lhs, std::string_view rhs) {
    return lhs.view() == rhs;
  }
  friend bool operator==(std::string_view lhs, Symbol rhs) {
    return rhs == lhs;
  }
  friend bool operator==(Symbol lhs, const char *rhs) {
    return lhs.view() == std::string_view(rhs ? rhs : "");
  }
  friend bool operator==(const char *lhs, Symbol rhs) { return rhs == lhs; }
  friend bool operator!=(Symbol lhs, std::string_view rhs) {
    return !(lhs == rhs);
  }
  friend bool operator!=(std::string_view lhs, Symbol rhs) {
    return !(lhs == rhs);
  }
  friend bool operator!=(Symbol lhs, const char *rhs) { return !(lhs == rhs); }
  friend bool operator!=(const char *lhs, Symbol rhs) { return !(lhs == rhs); }

private:
  struct Table {
    std::mutex mu;
    std::unordered_map<std::string_view, uint32_t> ids;
    std::deque<std::string> strings;

    Table() {
      strings.emplace_back("");
      ids.emplace(strings.back(), 0);
    }
  };

  static Table &table() {
    static Table tbl;
    return tbl;
  }

  static uint32_t intern(std::string_view text) {
    Table &tbl = table();
    std::lock_guard<std::mutex> lock(tbl.mu);
    auto it = tbl.ids.find(text);
    if (it != tbl.ids.end()) {
      return it->second;
    }

    uint32_t id = static_cast<uint32_t>(tbl.strings.size());
    tbl.strings.emplace_back(text);
    tbl.ids.emplace(tbl.strings.back(), id);
    return id;
  }

  uint32_t id_ = 0;
};

inline std::ostream &operator<<(std::ostream &os, Symbol sym) {
  os << sym.view();
  return os;
}

using Duration = std::chrono::steady_clock::duration;
using Instant = std::chrono::steady_clock::time_point;

inline Instant now() { return std::chrono::steady_clock::now(); }

template <typename T> inline void hashCombine(size_t &seed, const T &value) {
  seed ^=
      std::hash<T>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

struct PairHash {
  template <typename A, typename B>
  size_t operator()(const std::pair<A, B> &value) const noexcept {
    size_t seed = 0;
    hashCombine(seed, value.first);
    hashCombine(seed, value.second);
    return seed;
  }
};

inline std::string trim(std::string_view input) {
  size_t start = 0;
  while (start < input.size() &&
         std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }

  size_t end = input.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }

  return std::string(input.substr(start, end - start));
}

template <typename Range>
inline std::string joinStrings(const Range &items, std::string_view sep) {
  std::ostringstream oss;
  bool first = true;
  for (const auto &item : items) {
    if (!first) {
      oss << sep;
    }
    first = false;
    oss << item;
  }
  return oss.str();
}

template <typename T> class UniqueQueue {
public:
  void insert(const T &value) {
    auto [it, inserted] = set_.insert(value);
    if (inserted) {
      queue_.push_back(*it);
    }
  }

  template <typename It> void extend(It begin, It end) {
    for (auto it = begin; it != end; ++it) {
      insert(*it);
    }
  }

  template <typename Range> void extend(const Range &range) {
    extend(range.begin(), range.end());
  }

  std::optional<T> pop() {
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = queue_.front();
    queue_.pop_front();
    set_.erase(value);
    return value;
  }

  bool empty() const { return queue_.empty(); }
  size_t size() const { return queue_.size(); }
  void clear() {
    queue_.clear();
    set_.clear();
  }
  auto begin() const { return queue_.begin(); }
  auto end() const { return queue_.end(); }

private:
  std::unordered_set<T> set_;
  std::deque<T> queue_;
};

template <> class UniqueQueue<Id> {
public:
  void insert(Id value) {
    if (present_.size() <= value.index()) {
      present_.resize(value.index() + 1, false);
    }
    if (present_[value.index()]) {
      return;
    }
    present_[value.index()] = true;
    queue_.push_back(value);
  }

  template <typename It> void extend(It begin, It end) {
    for (auto it = begin; it != end; ++it) {
      insert(*it);
    }
  }

  template <typename Range> void extend(const Range &range) {
    extend(range.begin(), range.end());
  }

  std::optional<Id> pop() {
    if (queue_.empty()) {
      return std::nullopt;
    }
    Id value = queue_.front();
    queue_.pop_front();
    present_[value.index()] = false;
    return value;
  }

  bool empty() const { return queue_.empty(); }
  size_t size() const { return queue_.size(); }
  void clear() {
    for (Id id : queue_) {
      present_[id.index()] = false;
    }
    queue_.clear();
  }
  auto begin() const { return queue_.begin(); }
  auto end() const { return queue_.end(); }

private:
  std::vector<uint8_t> present_;
  std::deque<Id> queue_;
};

} // namespace lotus::egraph

template <> struct std::hash<lotus::egraph::Symbol> {
  size_t operator()(const lotus::egraph::Symbol &value) const noexcept {
    return std::hash<uint32_t>{}(value.id());
  }
};
