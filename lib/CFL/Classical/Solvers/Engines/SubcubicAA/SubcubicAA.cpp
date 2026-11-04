#include "CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAA.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <numeric>
#include <stdexcept>
#include <string>

namespace lotus::cfl::classical::engines::subcubic {
namespace {

using Word = std::uint64_t;
constexpr std::size_t WordBits = 64;

unsigned firstBit(Word bits) {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_ctzll(bits));
#else
  unsigned result = 0;
  while ((bits & 1) == 0) {
    bits >>= 1;
    ++result;
  }
  return result;
#endif
}

std::size_t bitCount(Word bits) {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::size_t>(__builtin_popcountll(bits));
#else
  std::size_t result = 0;
  while (bits) {
    bits &= bits - 1;
    ++result;
  }
  return result;
#endif
}

// Section 3.4's characteristic vectors. Empty rows do not allocate words.
// Dense local IDs make each difference O(ceil(component_size / WordBits) +
// output). With word size Theta(log n), this is the paper's fast-set bound.
class FastSet {
public:
  explicit FastSet(std::size_t nodes = 0)
      : word_count_(nodes / WordBits + (nodes % WordBits != 0)) {}

  bool contains(Node node) const {
    return !words_.empty() &&
           (words_[node / WordBits] & (Word{1} << (node % WordBits)));
  }

  bool insert(Node node) {
    if (words_.empty()) {
      words_.resize(word_count_, 0);
    }
    auto &word = words_[node / WordBits];
    const Word bit = Word{1} << (node % WordBits);
    const bool fresh = (word & bit) == 0;
    word |= bit;
    return fresh;
  }

  template <typename Visitor> bool visit(Visitor &&visitor) const {
    for (std::size_t index = 0; index < words_.size(); ++index) {
      Word bits = words_[index];
      while (bits) {
        const Node node = index * WordBits + firstBit(bits);
        bits &= bits - 1;
        if (!visitor(node)) {
          return false;
        }
      }
    }
    return true;
  }

  template <typename Visitor>
  void difference(const FastSet &known, bool fast, Statistics &stats,
                  Visitor &&visitor) const {
    if (!fast) {
      visit([&](Node node) {
        ++stats.candidate_checks;
        if (!known.contains(node)) {
          visitor(node);
        }
        return true;
      });
      return;
    }
    for (std::size_t index = 0; index < words_.size(); ++index) {
      ++stats.difference_words;
      Word bits = words_[index] & ~known.word(index);
      while (bits) {
        const Node node = index * WordBits + firstBit(bits);
        bits &= bits - 1;
        visitor(node);
      }
    }
  }

  std::size_t unite(const FastSet &other, bool fast, Statistics &stats) {
    if (this == &other || other.words_.empty()) {
      return 0;
    }
    std::size_t added = 0;
    if (!fast) {
      other.difference(*this, false, stats, [&](Node node) {
        added += insert(node);
      });
      return added;
    }
    if (words_.empty()) {
      words_.resize(word_count_, 0);
    }
    for (std::size_t index = 0; index < words_.size(); ++index) {
      ++stats.difference_words;
      const Word fresh = other.words_[index] & ~words_[index];
      added += bitCount(fresh);
      words_[index] |= fresh;
    }
    return added;
  }

private:
  Word word(std::size_t index) const {
    return words_.empty() ? 0 : words_[index];
  }
  std::size_t word_count_ = 0;
  std::vector<Word> words_;
};

class Matrix {
public:
  explicit Matrix(std::size_t nodes = 0) : rows_(nodes, FastSet(nodes)) {}

  bool insert(Node source, Node target) {
    const bool fresh = rows_[source].insert(target);
    count_ += fresh;
    return fresh;
  }
  bool contains(Node source, Node target) const {
    return rows_[source].contains(target);
  }
  const FastSet &row(Node source) const { return rows_[source]; }
  void uniteRow(Node source, const FastSet &other, bool fast,
                Statistics &stats) {
    count_ += rows_[source].unite(other, fast, stats);
  }
  std::size_t count() const { return count_; }

private:
  std::vector<FastSet> rows_;
  std::size_t count_ = 0;
};

struct IndexedRelation {
  explicit IndexedRelation(std::size_t nodes = 0) : out(nodes), in(nodes) {}
  Matrix out;
  Matrix in;
};

enum State : unsigned {
  M,
  V1,
  V1Prime,
  D1Prime,
  D1,
  D2Prime,
  D2,
  Invalid,
};
enum Label : unsigned { A, Abar, D, Memory };

// Figures 10 and 11. Prime states prevent consecutive M subpaths; D2 states
// cannot return to the reverse-assignment part of a path.
constexpr State Transitions[Invalid][4] = {
    {V1, Invalid, D1Prime, Invalid},
    {V1, Invalid, D1Prime, V1Prime},
    {V1, Invalid, D1Prime, Invalid},
    {D2, D1, M, Invalid},
    {D2, D1, M, D1Prime},
    {D2, Invalid, M, Invalid},
    {D2, Invalid, M, D2Prime},
};

struct WorkItem {
  State state;
  Node source;
  Node target;
};

struct ComponentInput {
  std::vector<Node> nodes;
  std::vector<std::pair<Node, Node>> assignments;
  std::vector<std::pair<Node, Node>> dereferences;
};

struct ComponentResult {
  std::vector<Node> nodes;
  Matrix memory;
  Matrix value;
};

class ComponentSolver {
public:
  ComponentSolver(const ComponentInput &input, const Options &options,
                  Statistics &stats)
      : nodes_(input.nodes.size()), options_(options), stats_(stats),
        assignments_(nodes_), reverse_assignments_(nodes_), dereferences_(nodes_),
        summaries_{IndexedRelation(nodes_), IndexedRelation(nodes_),
                   IndexedRelation(nodes_), IndexedRelation(nodes_),
                   IndexedRelation(nodes_), IndexedRelation(nodes_),
                   IndexedRelation(nodes_)} {
    for (const auto &edge : input.assignments) {
      assignments_.insert(edge.first, edge.second);
      reverse_assignments_.insert(edge.second, edge.first);
    }
    for (const auto &edge : input.dereferences) {
      dereferences_.insert(edge.first, edge.second);
    }
  }

  ComponentResult run() {
    computeMemory();
    // Algorithm 2 needs only M and V1. Release all reverse indices and pivot
    // relations before allocating its forward-path and output matrices.
    for (auto &relation : summaries_) {
      relation.in = Matrix();
    }
    for (unsigned state = V1Prime; state < Invalid; ++state) {
      summaries_[state].out = Matrix();
    }
    Matrix value = options_.compute_value_aliases ? computeValue() : Matrix();
    return {{}, std::move(summaries_[M].out), std::move(value)};
  }

private:
  bool addSummary(State state, Node source, Node target) {
    auto &relation = summaries_[state];
    if (!relation.out.insert(source, target)) {
      return false;
    }
    relation.in.insert(target, source);
    worklist_.push_back({state, source, target});
    stats_.peak_worklist = std::max(stats_.peak_worklist, worklist_.size());
    return true;
  }

  void addMemory(Node source, Node target) {
    addSummary(M, source, target);
    addSummary(M, target, source);
  }

  const FastSet &outgoing(Label label, Node source) const {
    switch (label) {
    case A:
      return assignments_.row(source);
    case Abar:
      return reverse_assignments_.row(source);
    case D:
      return dereferences_.row(source);
    case Memory:
      return summaries_[M].out.row(source);
    }
    throw std::logic_error("Invalid subcubic alias transition label");
  }

  void computeMemory() {
    // Algorithm 1, lines 1-3. A canonical PEG has at most one d successor.
    // Pairing all co-successors also handles dbar epsilon d exactly when a
    // supplied graph has multiple dereferences from the same expression.
    for (Node parent = 0; parent < nodes_; ++parent) {
      dereferences_.row(parent).visit([&](Node source) {
        dereferences_.row(parent).difference(
            summaries_[M].out.row(source), options_.use_fast_sets, stats_,
            [&](Node target) { addMemory(source, target); });
        return true;
      });
    }

    while (!worklist_.empty()) {
      const WorkItem item = worklist_.front();
      worklist_.pop_front();
      ++stats_.processed_items;
      const bool phase_one = item.state <= V1Prime;
      const Node endpoint = phase_one ? item.target : item.source;
      for (unsigned label = A; label <= Memory; ++label) {
        const State next = Transitions[item.state][label];
        if (next == Invalid) {
          continue;
        }
        const FastSet &known = phase_one
                                   ? summaries_[next].out.row(item.source)
                                   : summaries_[next].in.row(item.target);
        // Section 3.4: compare outgoing facts against Out(u,Y) in phase one,
        // and against In(v,Y) in phase two, before enumerating candidates.
        outgoing(static_cast<Label>(label), endpoint)
            .difference(known, options_.use_fast_sets, stats_, [&](Node node) {
              const Node source = phase_one ? item.source : node;
              const Node target = phase_one ? node : item.target;
              if (next == M) {
                addMemory(source, target);
              } else {
                addSummary(next, source, target);
              }
            });
      }
    }
  }

  Matrix computeValue() {
    Matrix &v1 = summaries_[V1].out;
    const Matrix &memory = summaries_[M].out;

    // Algorithm 2, line 2: only V1 is transitive. Bitset Warshall closes it
    // without enumerating an already-known Cartesian product.
    for (Node middle = 0; middle < nodes_; ++middle) {
      for (Node source = 0; source < nodes_; ++source) {
        if (v1.contains(source, middle)) {
          v1.uniteRow(source, v1.row(middle), options_.use_fast_sets, stats_);
        }
      }
    }

    // Lines 3-8: Gamma01 = M | V1 | V1 M. Gamma2 = a Gamma01 is the
    // forward-only relation called V2 in the paper's worklist Wv.
    Matrix gamma01 = memory;
    for (Node source = 0; source < nodes_; ++source) {
      gamma01.uniteRow(source, v1.row(source), options_.use_fast_sets, stats_);
      v1.row(source).visit([&](Node middle) {
        gamma01.uniteRow(source, memory.row(middle), options_.use_fast_sets,
                         stats_);
        return true;
      });
    }
    Matrix gamma2(nodes_);
    for (Node source = 0; source < nodes_; ++source) {
      assignments_.row(source).visit([&](Node middle) {
        gamma2.uniteRow(source, gamma01.row(middle), options_.use_fast_sets,
                        stats_);
        return true;
      });
    }
    // Keep Gamma and Gamma2 immutable throughout the final pairing. Feeding
    // newly derived alias pairs back as Gamma2 paths would incorrectly make
    // the result transitive. Identity accounts for zero-length Gamma paths.
    Matrix gamma = std::move(gamma01);
    for (Node source = 0; source < nodes_; ++source) {
      gamma.insert(source, source);
      gamma.uniteRow(source, gamma2.row(source), options_.use_fast_sets, stats_);
    }
    v1 = Matrix();

    Matrix value = memory;
    for (Node source = 0; source < nodes_; ++source) {
      value.insert(source, source);
    }
    // Lines 9-13: for each Gamma2(x,v), pair v with the Gamma successors of
    // x. Output is symmetric; Out(v,V) is therefore also its predecessor row.
    // Include both directions explicitly so ordered queries implement all V.
    for (Node source = 0; source < nodes_; ++source) {
      gamma2.row(source).visit([&](Node target) {
        gamma.row(source).difference(
            value.row(target), options_.use_fast_sets, stats_, [&](Node other) {
              value.insert(other, target);
              value.insert(target, other);
            });
        return true;
      });
    }
    return value;
  }

  std::size_t nodes_;
  const Options &options_;
  Statistics &stats_;
  Matrix assignments_;
  Matrix reverse_assignments_;
  Matrix dereferences_;
  std::array<IndexedRelation, Invalid> summaries_;
  std::deque<WorkItem> worklist_;
};

class DisjointSets {
public:
  explicit DisjointSets(std::size_t nodes) : parents_(nodes), sizes_(nodes, 1) {
    std::iota(parents_.begin(), parents_.end(), 0);
  }
  Node root(Node node) {
    while (parents_[node] != node) {
      parents_[node] = parents_[parents_[node]];
      node = parents_[node];
    }
    return node;
  }
  void unite(Node lhs, Node rhs) {
    lhs = root(lhs);
    rhs = root(rhs);
    if (lhs == rhs) {
      return;
    }
    if (sizes_[lhs] < sizes_[rhs]) {
      std::swap(lhs, rhs);
    }
    parents_[rhs] = lhs;
    sizes_[lhs] += sizes_[rhs];
  }

private:
  std::vector<Node> parents_;
  std::vector<std::size_t> sizes_;
};

void normalizeEdges(std::vector<std::pair<Node, Node>> &edges,
                    std::size_t nodes) {
  for (const auto &edge : edges) {
    if (edge.first >= nodes || edge.second >= nodes) {
      throw std::out_of_range("Subcubic alias edge endpoint is out of range");
    }
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
}

} // namespace

struct Result::Impl {
  Statistics stats;
  bool values_computed = false;
  std::vector<std::size_t> component_of;
  std::vector<Node> local_of;
  std::vector<ComponentResult> components;

  void requireNode(Node node) const {
    if (node >= stats.nodes) {
      throw std::out_of_range("Subcubic alias query node is out of range");
    }
  }
  void requireValues() const {
    if (!values_computed) {
      throw std::logic_error("Value aliases were not computed (memory-only run)");
    }
  }
  bool contains(Node source, Node target, bool value) const {
    requireNode(source);
    requireNode(target);
    if (component_of[source] != component_of[target]) {
      return false;
    }
    const auto &component = components[component_of[source]];
    const Matrix &relation = value ? component.value : component.memory;
    return relation.contains(local_of[source], local_of[target]);
  }
  bool visitSuccessors(Node source, bool value,
                       const NodeVisitor &visitor) const {
    requireNode(source);
    const auto &component = components[component_of[source]];
    const Matrix &relation = value ? component.value : component.memory;
    return relation.row(local_of[source]).visit(
        [&](Node target) { return visitor(component.nodes[target]); });
  }
  bool visitEdges(bool value, const EdgeVisitor &visitor) const {
    for (const auto &component : components) {
      const Matrix &relation = value ? component.value : component.memory;
      for (Node source = 0; source < component.nodes.size(); ++source) {
        if (!relation.row(source).visit([&](Node target) {
              return visitor(component.nodes[source], component.nodes[target]);
            })) {
          return false;
        }
      }
    }
    return true;
  }
};

Result::Result(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Result::Result(Result &&) noexcept = default;
Result &Result::operator=(Result &&) noexcept = default;
Result::~Result() = default;

const Result::Impl &Result::get() const {
  if (!impl_) {
    throw std::logic_error("Cannot query a moved-from subcubic alias result");
  }
  return *impl_;
}

std::size_t Result::nodeCount() const { return get().stats.nodes; }
bool Result::valueAliasesComputed() const { return get().values_computed; }
bool Result::memoryAlias(Node source, Node target) const {
  return get().contains(source, target, false);
}
bool Result::valueAlias(Node source, Node target) const {
  get().requireValues();
  return get().contains(source, target, true);
}
std::size_t Result::memoryAliasCount() const { return get().stats.memory_aliases; }
std::size_t Result::valueAliasCount() const {
  get().requireValues();
  return get().stats.value_aliases;
}
bool Result::visitMemorySuccessors(Node source,
                                   const NodeVisitor &visitor) const {
  return get().visitSuccessors(source, false, visitor);
}
bool Result::visitValueSuccessors(Node source, const NodeVisitor &visitor) const {
  get().requireValues();
  return get().visitSuccessors(source, true, visitor);
}
bool Result::visitMemoryAliases(const EdgeVisitor &visitor) const {
  return get().visitEdges(false, visitor);
}
bool Result::visitValueAliases(const EdgeVisitor &visitor) const {
  get().requireValues();
  return get().visitEdges(true, visitor);
}
const Statistics &Result::statistics() const { return get().stats; }

Result solve(const Problem &problem, const Options &options) {
  auto assignments = problem.assignments;
  auto dereferences = problem.dereferences;
  normalizeEdges(assignments, problem.nodes);
  normalizeEdges(dereferences, problem.nodes);
  std::vector<bool> has_dereference_parent(problem.nodes, false);
  for (const auto &edge : dereferences) {
    has_dereference_parent[edge.second] = true;
  }
  for (const auto &edge : assignments) {
    if (!has_dereference_parent[edge.second]) {
      throw std::invalid_argument(
          "Subcubic alias analysis requires every assignment target to have "
          "an incoming d edge; target " + std::to_string(edge.second) +
          " does not satisfy the PEG condition");
    }
  }

  auto impl = std::make_unique<Result::Impl>();
  impl->stats.nodes = problem.nodes;
  impl->stats.assignment_edges = assignments.size();
  impl->stats.dereference_edges = dereferences.size();
  impl->values_computed = options.compute_value_aliases;
  impl->component_of.resize(problem.nodes);
  impl->local_of.resize(problem.nodes);

  DisjointSets sets(problem.nodes);
  if (options.decompose_components) {
    for (const auto &edge : assignments) {
      sets.unite(edge.first, edge.second);
    }
    for (const auto &edge : dereferences) {
      sets.unite(edge.first, edge.second);
    }
  }
  std::vector<std::size_t> root_component(problem.nodes, problem.nodes);
  std::vector<ComponentInput> inputs;
  for (Node node = 0; node < problem.nodes; ++node) {
    const Node root = options.decompose_components ? sets.root(node) : 0;
    auto &index = root_component[root];
    if (index == problem.nodes) {
      index = inputs.size();
      inputs.emplace_back();
    }
    impl->component_of[node] = index;
    impl->local_of[node] = inputs[index].nodes.size();
    inputs[index].nodes.push_back(node);
  }
  for (const auto &edge : assignments) {
    inputs[impl->component_of[edge.first]].assignments.emplace_back(
        impl->local_of[edge.first], impl->local_of[edge.second]);
  }
  for (const auto &edge : dereferences) {
    inputs[impl->component_of[edge.first]].dereferences.emplace_back(
        impl->local_of[edge.first], impl->local_of[edge.second]);
  }
  impl->stats.connected_components = inputs.size();
  impl->components.reserve(inputs.size());
  for (auto &input : inputs) {
    impl->stats.largest_component =
        std::max(impl->stats.largest_component, input.nodes.size());
    auto component = ComponentSolver(input, options, impl->stats).run();
    component.nodes = std::move(input.nodes);
    impl->stats.memory_aliases += component.memory.count();
    impl->stats.value_aliases += component.value.count();
    impl->components.push_back(std::move(component));
  }
  return Result(std::move(impl));
}

} // namespace lotus::cfl::classical::engines::subcubic
