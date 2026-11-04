#include "CFL/InterleavedDyck/StagedBounds/CnfGraph.h"

#include "CFL/InterleavedDyck/StagedBounds/CnfGrammar.h"
#include "CFL/InterleavedDyck/StagedBounds/CnfTypes.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement {

std::uint64_t CnfGraph::RelationRow::wordAt(int index) const {
  if (!words.empty() && sparse.empty()) {
    return words[index];
  }
  if (!words.empty()) {
    const auto it = std::lower_bound(sparse.begin(), sparse.end(), index);
    return it != sparse.end() && *it == index ? words[it - sparse.begin()] : 0;
  }
  std::uint64_t result = 0;
  auto it = std::lower_bound(sparse.begin(), sparse.end(), index * 64);
  while (it != sparse.end() && *it / 64 == index) {
    result |= std::uint64_t{1} << (*it % 64);
    ++it;
  }
  return result;
}

bool CnfGraph::RelationRow::contains(int vertex) const {
  if (!words.empty()) {
    return (wordAt(vertex / 64) & (std::uint64_t{1} << (vertex % 64))) != 0;
  }
  return std::binary_search(sparse.begin(), sparse.end(), vertex);
}

bool CnfGraph::RelationRow::insert(int vertex, std::size_t vertex_count) {
  const auto mask = std::uint64_t{1} << (vertex % 64);
  if (!words.empty() && sparse.empty()) {
    auto &word = words[static_cast<std::size_t>(vertex) / 64];
    if ((word & mask) != 0) {
      return false;
    }
    word |= mask;
  } else if (!words.empty()) {
    const int index = vertex / 64;
    const auto it = std::lower_bound(sparse.begin(), sparse.end(), index);
    const auto offset = it - sparse.begin();
    if (it != sparse.end() && *it == index) {
      if ((words[offset] & mask) != 0) {
        return false;
      }
      words[offset] |= mask;
    } else {
      sparse.insert(it, index);
      words.insert(words.begin() + offset, mask);
    }
  } else {
    const auto it = std::lower_bound(sparse.begin(), sparse.end(), vertex);
    if (it != sparse.end() && *it == vertex) {
      return false;
    }
    sparse.insert(it, vertex);
    if (sparse.size() >= 32) {
      std::vector<int> indices;
      for (int target : sparse) {
        const int index = target / 64;
        if (indices.empty() || indices.back() != index) {
          indices.push_back(index);
          words.push_back(0);
        }
        words.back() |= std::uint64_t{1} << (target % 64);
      }
      sparse = std::move(indices);
    }
  }
  const std::size_t word_count = (vertex_count + 63) / 64;
  if (!words.empty() && !sparse.empty() &&
      sparse.size() >= (word_count + 1) / 2) {
    std::vector<std::uint64_t> dense(word_count, 0);
    for (std::size_t index = 0; index < sparse.size(); ++index) {
      dense[sparse[index]] = words[index];
    }
    words = std::move(dense);
    std::vector<int>().swap(sparse);
  }
  ++count;
  return true;
}

std::size_t CnfGraph::RelationRow::traceSlot(int vertex) const {
  if (!words.empty() && sparse.empty()) {
    return static_cast<std::size_t>(vertex);
  }
  const int key = words.empty() ? vertex : vertex / 64;
  const auto offset =
      std::lower_bound(sparse.begin(), sparse.end(), key) - sparse.begin();
  return words.empty() ? static_cast<std::size_t>(offset)
                       : offset * 64 + vertex % 64;
}

template <class Fn> void CnfGraph::RelationRow::forEachWord(Fn &&fn) const {
  if (!words.empty()) {
    for (std::size_t index = 0; index < words.size(); ++index) {
      if (words[index] != 0) {
        fn(sparse.empty() ? static_cast<int>(index) : sparse[index],
           words[index]);
      }
    }
    return;
  }
  for (std::size_t begin = 0; begin < sparse.size();) {
    const int index = sparse[begin] / 64;
    std::uint64_t bits = 0;
    do {
      bits |= std::uint64_t{1} << (sparse[begin++] % 64);
    } while (begin < sparse.size() && sparse[begin] / 64 == index);
    fn(index, bits);
  }
}

template <class Fn> void CnfGraph::RelationRow::forEach(Fn &&fn) const {
  if (words.empty()) {
    for (int vertex : sparse) {
      fn(vertex);
    }
    return;
  }
  forEachWord([&](int index, std::uint64_t bits) {
    while (bits != 0) {
      fn(index * 64 + __builtin_ctzll(bits));
      bits &= bits - 1;
    }
  });
}

template <class Fn>
void CnfGraph::RelationRow::forEachCommon(const RelationRow &other,
                                          Fn &&fn) const {
  forEachWord([&](int index, std::uint64_t bits) {
    bits &= other.wordAt(index);
    while (bits != 0) {
      fn(index * 64 + __builtin_ctzll(bits));
      bits &= bits - 1;
    }
  });
}

template <class Fn>
void CnfGraph::RelationRow::forEachMissing(const RelationRow *known,
                                           Fn &&fn) const {
  if (known == this) {
    return;
  }
  // Word-level difference avoids enumerating the (often cubic number of)
  // already-known proofs. Compressed rows skip absent words on large graphs.
  forEachWord([&](int index, std::uint64_t bits) {
    if (known != nullptr) {
      bits &= ~known->wordAt(index);
    }
    while (bits != 0) {
      fn(index * 64 + __builtin_ctzll(bits));
      bits &= bits - 1;
    }
  });
}

void CnfGraph::reinit(int n,
                      const std::unordered_set<Edge, EdgeHasher> &edges) {
  vertexCount = static_cast<std::size_t>(n);
  relations.clear();
  outgoingSymbols.assign(n, {});
  incomingSymbols.assign(n, {});
  for (const Edge &edge : edges) {
    addEdge(edge);
  }
}

bool CnfGraph::insertEdge(const Edge &edge) {
  auto &relation = relations[std::get<1>(edge)];
  const int source = std::get<0>(edge);
  const int target = std::get<2>(edge);
  auto &out = relation.outgoing[source];
  if (!out.insert(target, vertexCount)) {
    return false;
  }
  if (out.count == 1) {
    outgoingSymbols[source].push_back(std::get<1>(edge));
  }
  auto &in = relation.incoming[target];
  in.insert(source, vertexCount);
  if (in.count == 1) {
    incomingSymbols[target].push_back(std::get<1>(edge));
  }
  return true;
}

void CnfGraph::addEdge(const Edge &edge) { insertEdge(edge); }

bool CnfGraph::hasEdge(const Edge &edge) const {
  if (std::get<0>(edge) < 0 || std::get<2>(edge) < 0 ||
      static_cast<std::size_t>(std::get<0>(edge)) >= vertexCount ||
      static_cast<std::size_t>(std::get<2>(edge)) >= vertexCount) {
    return false;
  }
  const auto relation = relations.find(std::get<1>(edge));
  if (relation == relations.end()) {
    return false;
  }
  const auto row = relation->second.outgoing.find(std::get<0>(edge));
  return row != relation->second.outgoing.end() &&
         row->second.contains(std::get<2>(edge));
}

CnfGraph::EdgeSet CnfGraph::runCFLReachability(const CnfGrammar &grammar) {
  const auto result = runCFLReachabilityCompact(grammar);
  return {result.begin(), result.end()};
}

CnfGraph::EdgeSet CnfGraph::runCFLReachability(const CnfGrammar &grammar,
                                               UnaryRecord &singleRecord,
                                               BinaryRecord &binaryRecord) {
  const auto result =
      runCFLReachabilityCompact(grammar, singleRecord, binaryRecord);
  return {result.begin(), result.end()};
}

std::vector<Edge>
CnfGraph::runCFLReachabilityCompact(const CnfGrammar &grammar) {
  return runCFLReachabilityCore(grammar, nullptr, nullptr);
}

std::vector<Edge>
CnfGraph::runCFLReachabilityCompact(const CnfGrammar &grammar,
                                    UnaryRecord &singleRecord,
                                    BinaryRecord &binaryRecord) {
  return runCFLReachabilityCore(grammar, &singleRecord, &binaryRecord);
}

void CnfGraph::runCFLReachabilityVisit(
    const CnfGrammar &grammar,
    const std::function<void(const Edge &)> &visitor) {
  runCFLReachabilityCore(grammar, nullptr, nullptr, &visitor);
}

CnfGraph::EdgeSet
CnfGraph::getEdgeClosure(const CnfGrammar &grammar, const EdgeSet &result,
                         const UnaryRecord &singleRecord,
                         const BinaryRecord &binaryRecord) const {
  return getEdgeClosureCompact(grammar, {result.begin(), result.end()},
                               singleRecord, binaryRecord);
}

CnfGraph::EdgeSet CnfGraph::getFactorizedEdgeClosure(const CnfGrammar &grammar,
                                                     const EdgeSet &result) {
  return getFactorizedEdgeClosureCompact(grammar,
                                         {result.begin(), result.end()});
}

std::unordered_set<Edge, EdgeHasher> CnfGraph::getEdgeClosureCompact(
    const CnfGrammar &grammar, const std::vector<Edge> &result,
    const std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>
        &singleRecord,
    const std::unordered_map<
        Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
        EdgeHasher> &binaryRecord) const {
  // The set of edges to be returned, which only contains original edges in the
  // graph
  std::unordered_set<Edge, EdgeHasher> closure;
  // The set to avoid visiting the same edge (including summary edges)  more
  // than once
  std::unordered_set<Edge, EdgeHasher> vis;
  std::deque<Edge> w;
  // Start from all S edges
  for (const Edge &e : result) {
    vis.insert(e);
    w.push_back(e);
  }
  while (!w.empty()) {
    Edge e = w.front();
    w.pop_front();
    // i --x--> j
    int i = std::get<0>(e);
    int x = std::get<1>(e);
    int j = std::get<2>(e);
    if (grammar.terminals.count(x) == 1) {
      // Only insert terminal (original) edges
      closure.insert(e);
    } else {
      // This is a const member function so we can't write singleRecord[e].
      if (singleRecord.count(e) == 1) {
        for (int y : singleRecord.at(e)) {
          Edge e1 = std::make_tuple(i, y, j);
          if (vis.count(e1) == 0) {
            vis.insert(e1);
            w.push_back(e1);
          }
        }
      }
      if (binaryRecord.count(e) == 1) {
        for (const auto &triple : binaryRecord.at(e)) {
          int y = std::get<0>(triple);
          int k = std::get<1>(triple);
          int z = std::get<2>(triple);
          Edge e1 = std::make_tuple(i, y, k);
          Edge e2 = std::make_tuple(k, z, j);
          if (vis.count(e1) == 0) {
            vis.insert(e1);
            w.push_back(e1);
          }
          if (vis.count(e2) == 0) {
            vis.insert(e2);
            w.push_back(e2);
          }
        }
      }
    }
  }
  return closure;
}

std::unordered_set<Edge, EdgeHasher>
CnfGraph::getFactorizedEdgeClosureCompact(const CnfGrammar &grammar,
                                          const std::vector<Edge> &result) {
  std::unordered_set<Edge, EdgeHasher> closure;
  if (result.empty()) {
    return closure;
  }

  // Both directions are already partitioned by symbol for saturation. Reuse
  // them and give each row a slice of one visited bitmap. Sparse rows use one
  // bit per fact; dense rows use one bit per possible endpoint.
  std::size_t slots = 0;
  std::size_t terminal_count = 0;
  std::unordered_map<int, std::size_t> remaining;
  std::vector<std::size_t> offsets;
  // A row has at most vertexCount (an int) distinct endpoints.
  std::vector<std::uint32_t> remaining_out, remaining_in;
  std::size_t out_rows = 0, in_rows = 0;
  for (const auto &entry : relations) {
    out_rows += entry.second.outgoing.size();
    in_rows += entry.second.incoming.size();
  }
  offsets.reserve(out_rows);
  remaining_out.reserve(out_rows);
  remaining_in.reserve(in_rows);
  for (auto &[symbol, relation] : relations) {
    std::size_t count = 0;
    for (auto &[source, row] : relation.outgoing) {
      (void)source;
      row.traceIndex = remaining_out.size();
      offsets.push_back(slots);
      remaining_out.push_back(static_cast<std::uint32_t>(row.count));
      slots += row.words.empty() ? row.count : row.words.size() * 64;
      count += row.count;
    }
    for (auto &[target, row] : relation.incoming) {
      (void)target;
      row.traceIndex = remaining_in.size();
      remaining_in.push_back(static_cast<std::uint32_t>(row.count));
    }
    remaining[symbol] = count;
    if (grammar.terminals.count(symbol) != 0U) {
      terminal_count += count;
    }
  }
  std::vector<bool> visited(slots, false);
  std::deque<Edge> worklist;
  const auto enqueue = [&](int source, int symbol, int target,
                           const RelationRow &row) {
    const auto slot = offsets[row.traceIndex] + row.traceSlot(target);
    if (visited[slot]) {
      return;
    }
    visited[slot] = true;
    --remaining.at(symbol);
    --remaining_out[row.traceIndex];
    --remaining_in[relations.at(symbol).incoming.at(target).traceIndex];
    const Edge edge{source, symbol, target};
    if (grammar.terminals.count(symbol) != 0U) {
      closure.insert(edge);
    } else {
      worklist.push_back(edge);
    }
  };
  for (const Edge &edge : result) {
    if (hasEdge(edge)) {
      enqueue(std::get<0>(edge), std::get<1>(edge), std::get<2>(edge),
              relations.at(std::get<1>(edge)).outgoing.at(std::get<0>(edge)));
    }
  }

  // Provenance is a union: once every original terminal edge is marked,
  // no further derivation can change the answer.
  while (!worklist.empty() && closure.size() != terminal_count) {
    const Edge edge = worklist.front();
    worklist.pop_front();
    const int source = std::get<0>(edge);
    const int symbol = std::get<1>(edge);
    const int target = std::get<2>(edge);
    const auto unary = grammar.unaryL.find(symbol);
    if (unary != grammar.unaryL.end()) {
      for (int production_index : unary->second) {
        const int child = grammar.unaryProductions[production_index].second;
        if (hasEdge({source, child, target})) {
          enqueue(source, child, target,
                  relations.at(child).outgoing.at(source));
        }
      }
    }
    const auto binary = grammar.binaryL.find(symbol);
    if (binary == grammar.binaryL.end()) {
      continue;
    }
    for (int production_index : binary->second) {
      const auto &production = grammar.binaryProductions[production_index];
      const int left = production.second.first;
      const int right = production.second.second;
      // All-pairs roots quickly exhaust S in the expensive S -> S S rule.
      // Already-demanded children need not be rediscovered through more pivots.
      if (remaining[left] == 0 && remaining[right] == 0) {
        continue;
      }
      const auto left_relation = relations.find(left);
      const auto right_relation = relations.find(right);
      if (left_relation == relations.end() ||
          right_relation == relations.end()) {
        continue;
      }
      const auto out = left_relation->second.outgoing.find(source);
      const auto in = right_relation->second.incoming.find(target);
      if (out == left_relation->second.outgoing.end() ||
          in == right_relation->second.incoming.end()) {
        continue;
      }
      // Irrelevant facts elsewhere in a symbol relation must not prevent
      // skipping a join whose two local rows are already fully demanded.
      if (remaining_out[out->second.traceIndex] == 0 &&
          remaining_in[in->second.traceIndex] == 0) {
        continue;
      }
      const auto visit_pivot = [&](int pivot) {
        enqueue(source, left, pivot, out->second);
        enqueue(pivot, right, target,
                right_relation->second.outgoing.at(pivot));
      };
      if (out->second.count <= in->second.count) {
        out->second.forEachCommon(in->second, visit_pivot);
      } else {
        in->second.forEachCommon(out->second, visit_pivot);
      }
    }
  }
  return closure;
}

std::vector<Edge> CnfGraph::runCFLReachabilityCore(
    const CnfGrammar &grammar,
    std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher> *singleRecord,
    std::unordered_map<
        Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
        EdgeHasher> *binaryRecord,
    const std::function<void(const Edge &)> *visitor) {
  std::deque<Edge> worklist;
  for (const auto &entry : relations) {
    const int symbol = entry.first;
    for (const auto &out : entry.second.outgoing) {
      const int source = out.first;
      out.second.forEach([&](int target) {
        const Edge edge{source, symbol, target};
        worklist.push_back(edge);
      });
    }
  }

  for (int symbol : grammar.emptyProductions) {
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
      const Edge edge{static_cast<int>(vertex), symbol,
                      static_cast<int>(vertex)};
      if (insertEdge(edge)) {
        worklist.push_front(edge);
      }
    }
  }

  // Index the grammar by each RHS position. A delta edge then visits only
  // matching symbol rows, rather than all labels incident at its endpoints.
  std::unordered_map<int, std::vector<int>> binary_left, binary_right;
  for (std::size_t index = 0; index < grammar.binaryProductions.size();
       ++index) {
    const auto &production = grammar.binaryProductions[index];
    binary_left[production.second.first].push_back(static_cast<int>(index));
    binary_right[production.second.second].push_back(static_cast<int>(index));
  }
  std::vector<Edge> pending;
  while (!worklist.empty()) {
    const Edge current = worklist.front();
    const int source = std::get<0>(current);
    const int symbol = std::get<1>(current);
    const int target = std::get<2>(current);
    worklist.pop_front();
    pending.clear();
    const auto unary = grammar.unaryR.find(symbol);
    if (unary != grammar.unaryR.end()) {
      for (int index : unary->second) {
        const int lhs = grammar.unaryProductions[index].first;
        const Edge edge{source, lhs, target};
        pending.push_back(edge);
        if (singleRecord != nullptr) {
          (*singleRecord)[edge].insert(symbol);
        }
      }
    }
    const auto left = binary_left.find(symbol);
    if (left != binary_left.end()) {
      const auto apply = [&](int index) {
        const auto &production = grammar.binaryProductions[index];
        const int lhs = production.first;
        const int rhs = production.second.second;
        const auto relation = relations.find(rhs);
        if (relation == relations.end()) {
          return;
        }
        const auto input = relation->second.outgoing.find(target);
        if (input == relation->second.outgoing.end()) {
          return;
        }
        const auto output_relation = relations.find(lhs);
        const RelationRow *known = nullptr;
        if (output_relation != relations.end()) {
          const auto output = output_relation->second.outgoing.find(source);
          if (output != output_relation->second.outgoing.end()) {
            known = &output->second;
          }
        }
        if (binaryRecord == nullptr) {
          input->second.forEachMissing(
              known, [&](int end) { pending.emplace_back(source, lhs, end); });
        } else {
          input->second.forEach([&](int end) {
            const Edge edge{source, lhs, end};
            if (known == nullptr || !known->contains(end)) {
              pending.push_back(edge);
            }
            (*binaryRecord)[edge].emplace(symbol, target, rhs);
          });
        }
      };
      // Choose the smaller join driver: grammar alternatives or symbols
      // actually present at this vertex. Large typed grammars can have many
      // S -> ... alternatives but very few incident matching labels.
      if (left->second.size() <= outgoingSymbols[target].size()) {
        for (int index : left->second) {
          apply(index);
        }
      } else {
        for (int other : outgoingSymbols[target]) {
          const auto matches = grammar.binaryR.find({symbol, other});
          if (matches != grammar.binaryR.end()) {
            for (int index : matches->second) {
              apply(index);
            }
          }
        }
      }
    }
    const auto right = binary_right.find(symbol);
    if (right != binary_right.end()) {
      const auto apply = [&](int index) {
        const auto &production = grammar.binaryProductions[index];
        const int lhs = production.first;
        const int rhs = production.second.first;
        const auto relation = relations.find(rhs);
        if (relation == relations.end()) {
          return;
        }
        const auto input = relation->second.incoming.find(source);
        if (input == relation->second.incoming.end()) {
          return;
        }
        const auto output_relation = relations.find(lhs);
        const RelationRow *known = nullptr;
        if (output_relation != relations.end()) {
          const auto output = output_relation->second.incoming.find(target);
          if (output != output_relation->second.incoming.end()) {
            known = &output->second;
          }
        }
        if (binaryRecord == nullptr) {
          input->second.forEachMissing(known, [&](int begin) {
            pending.emplace_back(begin, lhs, target);
          });
        } else {
          input->second.forEach([&](int begin) {
            const Edge edge{begin, lhs, target};
            if (known == nullptr || !known->contains(begin)) {
              pending.push_back(edge);
            }
            (*binaryRecord)[edge].emplace(rhs, source, symbol);
          });
        }
      };
      // Choose the smaller join driver: grammar alternatives or symbols
      // actually present at this vertex. Large typed grammars can have many
      // S -> ... alternatives but very few incident matching labels.
      if (right->second.size() <= incomingSymbols[source].size()) {
        for (int index : right->second) {
          apply(index);
        }
      } else {
        for (int other : incomingSymbols[source]) {
          const auto matches = grammar.binaryR.find({other, symbol});
          if (matches != grammar.binaryR.end()) {
            for (int index : matches->second) {
              apply(index);
            }
          }
        }
      }
    }
    // Defer mutation until row iteration ends: insertion can promote a row
    // from sparse storage to a bitmap, invalidating iterators into that row.
    for (const Edge &edge : pending) {
      if (insertEdge(edge)) {
        worklist.push_front(edge);
      }
    }
  }
  std::vector<Edge> result;
  const auto start = relations.find(grammar.startSymbol);
  if (start != relations.end()) {
    std::size_t count = 0;
    for (const auto &out : start->second.outgoing) {
      count += out.second.count;
    }
    if (visitor == nullptr) {
      result.reserve(count);
    }
    for (const auto &out : start->second.outgoing) {
      out.second.forEach([&](int target) {
        const Edge edge{out.first, grammar.startSymbol, target};
        if (visitor != nullptr) {
          (*visitor)(edge);
        } else {
          result.push_back(edge);
        }
      });
    }
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement
