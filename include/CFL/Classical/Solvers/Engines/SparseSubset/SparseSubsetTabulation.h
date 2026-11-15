// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::classical::engines::sparse_subset {

/// Bit-sliced vector of small unsigned counters. Bit i stores bit i of
/// every lane; vector addition is an exact lane-wise integer addition using
/// Boolean carry propagation (no cross-lane carries).
class BitPlaneCounters {
public:
  BitPlaneCounters() = default;
  BitPlaneCounters(std::size_t lanes, std::size_t max_value)
      : lanes_(lanes), planes_(std::max<std::size_t>(1, bitWidth(max_value)),
                              std::vector<std::uint64_t>((lanes+63)/64,0)) {}
  std::size_t size() const { return lanes_; }
  std::size_t planeCount() const { return planes_.size(); }
  std::uint32_t get(std::size_t lane) const {
    if (lane>=lanes_) throw std::out_of_range("bit-plane lane");
    std::uint32_t value=0;
    for(std::size_t b=0;b<planes_.size();++b)
      if((planes_[b][lane/64]>>(lane%64))&1ULL) value |= (1U<<b);
    return value;
  }
  void set(std::size_t lane,std::uint32_t value) {
    if(lane>=lanes_) throw std::out_of_range("bit-plane lane");
    for(std::size_t b=0;b<planes_.size();++b) {
      auto &word=planes_[b][lane/64];
      const auto mask=1ULL<<(lane%64);
      if((value>>b)&1U) word|=mask; else word&=~mask;
    }
  }
  void add(const BitPlaneCounters &other) {
    if(lanes_!=other.lanes_ || planes_.size()<other.planes_.size())
      throw std::invalid_argument("incompatible bit-plane vectors");
    std::vector<std::uint64_t> carry((lanes_+63)/64,0);
    for(std::size_t b=0;b<planes_.size();++b) {
      for(std::size_t w=0;w<carry.size();++w) {
        const auto a=planes_[b][w];
        const auto rhs=b<other.planes_.size()?other.planes_[b][w]:0;
        const auto sum=a^rhs;
        const auto c=(a&rhs) | (a&carry[w]) | (rhs&carry[w]);
        planes_[b][w]=sum^carry[w];
        carry[w]=c;
      }
    }
    if(std::any_of(carry.begin(),carry.end(),[](auto x){return x!=0;}))
      throw std::overflow_error("bit-plane counter overflow");
  }
private:
  static std::size_t bitWidth(std::size_t x) {
    std::size_t b=0; do {++b; x>>=1;} while(x); return b;
  }
  std::size_t lanes_=0;
  std::vector<std::vector<std::uint64_t>> planes_;
};

/// Exact sparse-subset tabulation for a monotonically growing Boolean matrix.
/// Each block stores sums for every non-empty coordinate subset of size <= s.
/// Incidence lists make a 0->1 matrix update touch exactly the subsets that
/// contain the changed coordinate.
class SparseSubsetTable {
public:
  using Coordinate = std::size_t;
  using Counts = BitPlaneCounters;

  SparseSubsetTable(std::size_t rows, std::size_t columns,
                    std::size_t block_size, std::size_t sparsity)
      : rows_(rows), columns_(columns),
        block_size_(std::max<std::size_t>(1, block_size)),
        sparsity_(std::max<std::size_t>(1, sparsity)),
        matrix_(rows, std::vector<std::uint64_t>((columns+63)/64, 0)),
        incidence_(columns) {
    buildBlocks();
  }

  bool set(std::size_t row, Coordinate column) {
    check(row, column);
    auto &matrix_word=matrix_[row][column/64];
    const auto matrix_mask=std::uint64_t{1}<<(column%64);
    if (matrix_word & matrix_mask) return false;
    matrix_word |= matrix_mask;
    const auto b = column / block_size_;
    for (const auto &key : incidence_[column]) {
      auto &v = tables_[b].at(key);
      v.set(row, v.get(row) + 1);
    }
    return true;
  }

  bool contains(std::size_t row, Coordinate column) const {
    check(row, column);
    return ((matrix_[row][column/64]>>(column%64))&1ULL)!=0;
  }

  /// Sum columns in a sorted set, split into <=s pieces inside each block.
  /// The returned vector is exact (not Booleanized).
  std::vector<std::pair<std::size_t, Counts>>
  query(const std::vector<Coordinate> &coordinates) const {
    std::vector<std::pair<std::size_t, Counts>> result;
    if (coordinates.empty()) return result;
    if (!std::is_sorted(coordinates.begin(), coordinates.end()) ||
        std::adjacent_find(coordinates.begin(), coordinates.end()) != coordinates.end())
      throw std::invalid_argument("sparse-table query must be sorted and unique");
    std::unordered_map<std::size_t, Counts> sums;
    std::size_t i = 0;
    while (i < coordinates.size()) {
      const auto b = coordinates[i] / block_size_;
      std::vector<std::size_t> piece;
      while (i < coordinates.size() &&
             coordinates[i] / block_size_ == b &&
             piece.size() < sparsity_) {
        if (!piece.empty() && coordinates[i] <= piece.back())
          throw std::invalid_argument("sparse-table query must be sorted and unique");
        piece.push_back(coordinates[i] % block_size_);
        ++i;
      }
      auto it = tables_[b].find(piece);
      if (it == tables_[b].end())
        throw std::logic_error("missing sparse subset vector");
      auto &sum = sums[b];
      if (sum.size() == 0) sum = Counts(rows_, std::max(coordinates.size(), sparsity_));
      sum.add(it->second);
    }
    for (auto &entry : sums) result.emplace_back(entry.first, std::move(entry.second));
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    return result;
  }

  std::size_t rows() const { return rows_; }
  std::size_t columns() const { return columns_; }
  std::size_t blockSize() const { return block_size_; }
  std::size_t sparsity() const { return sparsity_; }
  std::size_t subsetCount() const {
    std::size_t n = 0;
    for (const auto &b : tables_) n += b.size();
    return n;
  }

private:
  void check(std::size_t r, std::size_t c) const {
    if (r >= rows_ || c >= columns_) throw std::out_of_range("sparse table index");
  }
  static void enumerate(std::size_t L, std::size_t s, std::size_t next,
                        std::vector<std::size_t> &cur,
                        std::vector<std::vector<std::size_t>> &out) {
    if (!cur.empty()) out.push_back(cur);
    if (cur.size() == s) return;
    for (std::size_t x = next; x < L; ++x) {
      cur.push_back(x);
      enumerate(L, s, x + 1, cur, out);
      cur.pop_back();
    }
  }
  void buildBlocks() {
    const auto blocks = (columns_ + block_size_ - 1) / block_size_;
    tables_.resize(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
      const auto L = std::min(block_size_, columns_ - b * block_size_);
      std::vector<std::vector<std::size_t>> subsets;
      std::vector<std::size_t> cur;
      enumerate(L, std::min(sparsity_, L), 0, cur, subsets);
      for (const auto &key : subsets) {
        tables_[b].emplace(key, Counts(rows_, sparsity_));
        for (auto local : key)
          incidence_[b * block_size_ + local].push_back(key);
      }
    }
  }
  std::size_t rows_, columns_, block_size_, sparsity_;
  std::vector<std::vector<std::uint64_t>> matrix_;
  std::vector<std::map<std::vector<std::size_t>, Counts>> tables_;
  std::vector<std::vector<std::vector<std::size_t>>> incidence_;
};

/// Dense packed bit set used for current and snapshot lines. Iteration over
/// a group visits machine words and extracts set bits, matching the paper's
/// packed report scans.
class PackedSet {
public:
  explicit PackedSet(std::size_t n=0) : bits_((n+63)/64,0), universe_(n) {}
  bool insert(std::size_t x) {
    if(x>=universe_) throw std::out_of_range("packed-set index");
    auto &word=bits_[x/64]; const auto mask=std::uint64_t{1}<<(x%64);
    if(word&mask) return false;
    word|=mask; ++size_; return true;
  }
  bool contains(std::size_t x) const {
    if(x>=universe_) return false;
    return (bits_[x/64]>>(x%64))&1ULL;
  }
  std::size_t size() const { return size_; }
  std::uint64_t word(std::size_t i) const { return i<bits_.size()?bits_[i]:0; }
private:
  std::vector<std::uint64_t> bits_;
  std::size_t universe_=0, size_=0;
};

/// Exact two-sided incremental set-pair structure from the paper. The sparse
/// tables accelerate promotions; snapshots, pending lists, split counters and
/// live-group reporting implement the stated invariants.
class TwoSidedSetPair {
public:
  struct Options {
    std::size_t groups = 0;
    std::size_t promotion_threshold = 0;
    std::size_t block_size = 0;
    std::size_t sparsity = 0;
  };
  explicit TwoSidedSetPair(std::size_t n) : TwoSidedSetPair(n, Options{}) {}
  TwoSidedSetPair(std::size_t n, Options o)
      : actual_n_(n),
        g_(o.groups ? o.groups : std::max<std::size_t>(1, ceilLog2(n))),
        D_(o.promotion_threshold ? o.promotion_threshold :
           std::max<std::size_t>(1, n / std::max<std::size_t>(1, ceilLog2(n)*ceilLog2(n)))),
        L_(o.block_size ? o.block_size : std::max<std::size_t>(1, n / std::max<std::size_t>(1,g_))),
        s_(o.sparsity ? o.sparsity : std::max<std::size_t>(1, ceilLog2(n)/2)),
        group_width_(((n + g_ - 1) / g_ + L_ - 1) / L_ * L_),
        n_(g_ * group_width_),
        x_(n_, PackedSet(n_)), y_(n_, PackedSet(n_)),
        x0_(n_, PackedSet(n_)), y0_(n_, PackedSet(n_)),
        dx_(n_), dy_(n_),
        tx_(n_,n_,L_,s_), ty_(n_,n_,L_,s_),
        cx_(g_, std::vector<std::size_t>(n_,0)),
        icol_(g_, std::vector<BitPlaneCounters>(n_, BitPlaneCounters(n_,n_))),
        irow_(g_, std::vector<BitPlaneCounters>(n_, BitPlaneCounters(n_,n_))) {
    if (!actual_n_) throw std::invalid_argument("set-pair universe must be nonempty");
  }
  bool insertX(std::size_t line, std::size_t element) { return insert(true,line,element); }
  bool insertY(std::size_t line, std::size_t element) { return insert(false,line,element); }

  std::vector<std::size_t> report(std::size_t v, std::size_t u) const {
    check(v,u);
    std::vector<bool> live(g_,false);
    for (auto x : dx_[v]) if (!y_[u].contains(x)) live[group(x)] = true;
    std::vector<std::size_t> h(g_,0);
    for (auto y : dy_[u]) if (x0_[v].contains(y)) ++h[group(y)];
    for (std::size_t g=0; g<g_; ++g) {
      const auto N = cx_[g][v] - icol_[g][u].get(v) - irow_[g][v].get(u);
      if (N > h[g]) live[g] = true;
    }
    std::vector<std::size_t> out;
    for (std::size_t g=0; g<g_; ++g) if (live[g]) {
      const auto lo = g*group_width_, hi = std::min(actual_n_, lo+group_width_);
      const auto first_word=lo/64, last_word=(hi+63)/64;
      for(std::size_t w=first_word;w<last_word;++w) {
        std::uint64_t bits=x_[v].word(w)&~y_[u].word(w);
        if(w==first_word && lo%64)
          bits &= (~std::uint64_t{0}) << (lo%64);
        if(w+1==last_word && hi%64)
          bits &= (std::uint64_t{1} << (hi%64))-1;
        while(bits) {
          const auto bit=static_cast<unsigned>(__builtin_ctzll(bits));
          const auto element=w*64+bit;
          if(element<hi) out.push_back(element);
          bits &= bits-1;
        }
      }
    }
    return out;
  }
  std::size_t count(std::size_t v, std::size_t u) const {
    check(v,u);
    std::size_t c=0;
    for (std::size_t g=0; g<g_; ++g) c += icol_[g][u].get(v)+irow_[g][v].get(u);
    for (auto x:dx_[v]) if (y_[u].contains(x)) ++c;
    for (auto y:dy_[u]) if (x0_[v].contains(y)) ++c;
    return c;
  }
  bool subset(std::size_t v,std::size_t u) const { return count(v,u)==x_[v].size(); }
  std::size_t universe() const { return actual_n_; }
  std::size_t promotionThreshold() const { return D_; }

private:
  static std::size_t ceilLog2(std::size_t x) {
    std::size_t r=0, v=x>1?x-1:0; while(v){++r;v>>=1;} return std::max<std::size_t>(1,r);
  }
  std::size_t group(std::size_t x) const { return std::min(g_-1,x/group_width_); }
  void check(std::size_t a,std::size_t b) const { if(a>=actual_n_||b>=actual_n_) throw std::out_of_range("set-pair index"); }
  bool insert(bool isX,std::size_t l,std::size_t e) {
    check(l,e);
    auto &family=isX?x_:y_; auto &pending=isX?dx_:dy_;
    if(!family[l].insert(e)) return false;
    pending[l].push_back(e);
    if(pending[l].size()>=D_) promote(isX,l);
    return true;
  }
  void promote(bool isX,std::size_t line) {
    auto &pending=isX?dx_[line]:dy_[line];
    auto &snapshot=isX?x0_[line]:y0_[line];
    auto R=pending; std::sort(R.begin(),R.end());
    auto updates=(isX?ty_:tx_).query(R);
    for (const auto &entry:updates) {
      const auto b=entry.first;
      // The paper chooses block/group boundaries together.  Keep that
      // invariant explicit: a block's coordinates belong to one group.
      const auto g=group(std::min(n_-1,b*L_));
      if(isX) irow_[g][line].add(entry.second);
      else icol_[g][line].add(entry.second);
    }
    for(auto e:R) {
      snapshot.insert(e);
      (isX? (void)tx_.set(line,e) : (void)ty_.set(line,e));
      if(isX) ++cx_[group(e)][line];
    }
    pending.clear();
  }
  std::size_t actual_n_,g_,D_,L_,s_,group_width_,n_;
  std::vector<PackedSet> x_,y_,x0_,y0_;
  std::vector<std::vector<std::size_t>> dx_,dy_;
  SparseSubsetTable tx_,ty_;
  std::vector<std::vector<std::size_t>> cx_;
  std::vector<std::vector<BitPlaneCounters>> icol_,irow_;
};

} // namespace lotus::cfl::classical::engines::sparse_subset
