#pragma once

// Graph-side interpreter for the boolean-matrix Kleene oracle: evaluates e-nodes
// directly on the e-graph so a test can assert that EVERY e-node in EVERY class
// agrees with its class representative — i.e. a rewrite never united a
// semantically different node into a class. Complements Ref-level evalRef.

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "BoolKleene.h"
#include "Dataflow/APA/EAN/AtomTable.h"
#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/PathLang.h"

namespace lotus_test_ean {

namespace ean = elimination::ean;

inline Mat evalClass(ean::Graph &g, const ean::AtomTable<int> &atoms,
                     ean::Id c, std::unordered_map<std::uint32_t, Mat> &memo);

inline Mat evalNode(ean::Graph &g, const ean::AtomTable<int> &atoms,
                    const ean::PathLang &n,
                    std::unordered_map<std::uint32_t, Mat> &memo) {
  if (ean::isZero(n)) return zeroM();
  if (ean::isOne(n)) return oneM();
  if (ean::isAtom(n)) return gen(*atoms.atom(ean::parseAtomId(n))->Transfer);
  if (ean::isStar(n)) return closureM(evalClass(g, atoms, n.children()[0], memo));
  if (ean::isJoin(n)) {
    Mat m = zeroM();
    for (ean::Id ch : n.children()) m = orM(m, evalClass(g, atoms, ch, memo));
    return m;
  }
  Mat m = oneM(); // seq
  for (ean::Id ch : n.children()) m = mulM(m, evalClass(g, atoms, ch, memo));
  return m;
}

inline Mat evalClass(ean::Graph &g, const ean::AtomTable<int> &atoms,
                     ean::Id c, std::unordered_map<std::uint32_t, Mat> &memo) {
  c = g.find(c);
  auto it = memo.find(c.value());
  if (it != memo.end()) return it->second;
  Mat m = evalNode(g, atoms, g[c].nodes.front(), memo);
  memo[c.value()] = m;
  return m;
}

// Every e-node in every class must interpret to its class representative.
inline bool allClassesConsistent(ean::Graph &g,
                                 const ean::AtomTable<int> &atoms) {
  std::unordered_map<std::uint32_t, Mat> memo;
  for (ean::Id c : g.classIds()) {
    (void)evalClass(g, atoms, g.find(c), memo);
  }
  for (ean::Id c : g.classIds()) {
    const ean::Id cc = g.find(c);
    const Mat ref = memo[cc.value()];
    for (const ean::PathLang &n : g[cc].nodes) {
      if (evalNode(g, atoms, n, memo) != ref) return false;
    }
  }
  return true;
}

} // namespace lotus_test_ean
