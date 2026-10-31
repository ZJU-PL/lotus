# POPL 2017 LCL interleaved-Dyck engine

This module implements the interleaved-Dyck instantiation of the reachability
algorithms in Qirun Zhang and Zhendong Su, **Context-Sensitive Data-Dependence
Analysis via Linear Conjunctive Language Reachability**, POPL 2017:
<https://helloqirun.github.io/papers/popl2017_qirun.pdf>.

The engine consumes `Core::Graph` (the actual type is
`lotus::cfl::interleaved_dyck::Graph`) and lives in
`lotus::cfl::interleaved_dyck::lcl`. It is independently implemented C++17, not a
copy of the authors' experimental artifact. It has no LLVM, Z3, or other
third-party implementation dependency; its only library dependency is
`CanaryInterleavedDyckCore`.

## Result contract

`Solver::analyze` returns an **all-pairs sound upper bound**, not an exact
interleaved-Dyck relation. This distinction is essential: an exact language
encoding does not make the paper's graph-reachability approximation exact.

* `result.mayReach(u, v) == false`: no interleaved-Dyck path exists in the supplied
  graph, provided the analysis completed successfully.
* `result.mayReach(u, v) == true`: a path may exist; the result is not a balanced
  witness or a certified lower bound.

There are two independent, typed stacks. An `OpenParenthesis(i)` must match a
`CloseParenthesis(i)` on the parenthesis stack, and similarly for brackets.
Operations on the two stacks can interleave. Label IDs are not collapsed to
one type. Directed edges are not symmetrized. Neutral edges mean epsilon.
Empty paths at every registered vertex and all neutral-only paths are included.

A small false-positive example, also covered by `KnownOverapproximation`, is:

```text
0 --ob--0--> 1 --cb--0--> 4 --cb--0--> 6
             \\--------------cb--1--> 6
```

The two words from 0 to 6 are `ob--0 cb--1` and `ob--0 cb--0 cb--0`.
Neither is balanced, but the default engine retains `(0,6)`. Abstract trellis
summaries can combine different overlapping paths; they do not carry a common
concrete path witness. Do not reinterpret `upper_bound` as exact reachability.

## API and build targets

Header: `CFL/InterleavedDyck/LCL/Solver.h`.
Library: `CanaryInterleavedDyckLCL`.
Executable: `lotus-cfl-interleaved-dyck lcl`.

```cpp
#include "CFL/InterleavedDyck/LCL/Solver.h"

namespace dyck = lotus::cfl::interleaved_dyck;

int main() {
  dyck::Graph graph;
  graph.addEdge(0, 1, dyck::Label::openParenthesis(1));
  graph.addEdge(1, 2, dyck::Label::openBracket(2));
  graph.addEdge(2, 3, dyck::Label::closeParenthesis(1));
  graph.addEdge(3, 4, dyck::Label::closeBracket(2));

  const auto result = dyck::lcl::Solver{}.analyze(graph);
  // This particular graph has a balanced crossing witness.
  // In general, a positive answer is only a candidate.
  return result.mayReach(0, 4) ? 0 : 1;
}
```

`Options` defaults to `Algorithm::Refined` with feasibility enabled. Select
`Algorithm::Baseline` for Algorithm 1; select `Algorithm::Refined` and set
`enable_feasibility = false` to isolate the gray-node refinement. The feasibility
option is ignored in baseline mode.

The result also contains counters for summaries, gray summaries, promotions,
left/right term updates, rule lookups, queue operations, epsilon pairs, and
normalized edges. A gray summary is counted only once, whether created gray or
promoted later. Baseline gray counters are zero. The solver is stateless, does
not modify its input, and may be reused after an exception. API queries for
unknown vertices return false; the CLI instead diagnoses an unknown query
vertex as an input error.

`max_summaries` and `max_normalized_edges` default to zero (unlimited). A positive
limit throws `std::length_error` when exceeded. **A resource-limited run never
returns an unfinished relation as an upper bound.** Invalid enum values throw
`std::invalid_argument`. Allocation failures propagate normally. These are fact
and edge limits, not a complete memory budget; epsilon closure and auxiliary
indexes also consume memory.

## Paper-to-code mapping

| Paper component | Implementation |
| --- | --- |
| Table 2, initialization rules (1)-(4) | `initialSummary` |
| Table 2, transition rules (5)-(17) | `findRule` |
| Algorithm 1, summary relation `S` | `Cell::summaries`, baseline mode |
| Algorithm 1, `L` and `R` term indexes | `Cell::left` and `Cell::right` |
| Algorithm 2, gray nodes and `L`/`L_b` | per-summary gray facts and `LeftTerm` masks |
| Algorithm 2, white-to-gray updates | `addSummary` promotion and worklist re-enqueue |
| Section 5.3, six feasibility conditions | `feasibleOutgoing`, `feasibleIncoming` |
| Accepting trellis state | `(Accept, #)`; gray in refined mode |

The two parts of a trellis state are separate: control `q` and work-tape symbol
`Z`. Controls are `q1`, `q2`, and typed seeking/deferred states for each alphabet.
Tape symbols are `#`, an open parenthesis, or an open bracket. Matching uses
`delta(Z_left, q_right)`; the unused fields of the two child summaries are not
unified. This is not a conventional CFL concatenation solver and is not an
intersection of independently computed Dyck reachability relations.

### Explicit implementation choices relative to printed Algorithm 2

The code implements a **monotone, order-independent interpretation** of the
paper's refined rules, not a literal transcription of every pseudocode branch.
In particular:

1. Summary validity is indexed by the complete `(q,Z)` state. An existing white
   summary can gain a gray derivation later; that promotion is re-enqueued so
   dependent bad left terms can become good ones. The implementation retains
   both the all-derivations fact and its good subset rather than deleting facts.
2. A term remembers a four-bit mask of its possible boundary label categories.
   Discovering a term along one parallel edge does not discard later feasible
   derivations along differently labeled edges.
3. Both incoming and outgoing boundary conditions are applied to a join, using
   retained existential witnesses. Filtering is independent of whether a left
   or right term happens to be discovered last. A rejected join does not mark
   its result permanently visited or suppress later valid witnesses.

These choices can change the candidate set relative to a literal execution of
the printed schedule-sensitive pseudocode. The implementation has not been
compared with the authors' artifact. It preserves the intended soundness
property and is checked against an independent synchronous implementation of
this documented fixed point.

### Fixed-point invariants

For a pair `(u,v)`, maintain white facts `W(u,v,q,Z)` and gray facts
`G(u,v,q,Z)`, with `G` a subset of `W`. Initial terminal edges create white
states; initial openers also create gray states. A processed summary from
`u` to `x`, extended by an original edge `x -a-> v`, contributes an all-left
fact `(Z, category(a))` at `(u,v)` and a good-left fact when that summary is
gray. A summary from `y` to `v`, preceded by an original edge `u -b-> y`,
contributes a right fact `(q, category(b))` at `(u,v)`.

A left and right fact at the same pair yield `delta(Z,q)`. Feasibility asks for
an allowed outgoing witness and an allowed incoming witness. The result is
gray only when its control is `q1` or `q2` and there is an allowed good-left
witness. All changes are insertions or monotone promotions; every new bit is
joined with the opposite term index. Thus each summary is processed at most
twice, and termination does not require a stack-depth or path-length cutoff.

For a genuine path, its trellis's two overlapping subpaths supply the required
left and right facts at each level. Genuine boundary labels satisfy the
necessary feasibility conditions, and genuine gray derivations propagate via
the good-left facts. Induction over the finite trellis therefore preserves its
accepting state. Abstract joins may combine unrelated witnesses, introducing
false positives but not dropping a genuine accepting derivation. The exact
neutral-edge transformation below extends this argument to epsilon paths.

## Neutral edges

Before saturation the engine computes epsilon closure `C = epsilon*` and
materializes terminal edges

```text
E' = C ; E_terminal ; C.
```

All epsilon-only pairs are inserted into the result separately. Neutral edges
are **not** treated as input occurrences of the tape marker `#`.
An epsilon-free fast path skips the closure computation entirely. The solver
keeps the original vertex identifiers, including isolated vertices, negative
IDs, and non-contiguous IDs. This preprocessing is exact but may substantially
increase the number of terminal edges.

The CLI reuses the existing Core DOT reader; it is not a general Graphviz
parser. Use one edge per line and labels such as `op--7`, `cp--7`, `ob--9`,
`cb--9`, or `normal`/`eps`/`epsilon`. The existing parser does not register
standalone vertex declarations. Represent isolated vertices through
`Graph::addVertex`, or use a neutral self-edge in CLI input.

## Complexity

For a fixed alphabet and epsilon-free input, saturation takes expected
`O(N*M)` time and `O(N^2)` fact space, in addition to storing input and output.
Initialization and reflexive output add `O(N+M)` work. The implementation uses
hash tables, so these are expected costs, not collision-independent worst-case
guarantees.

Alphabet size must not be hidden when it varies with the input. With `p`
parenthesis types and `b` bracket types, there are at most
`Q = 2 + 2*p + 2*b` controls, `Z = 1 + p + b` tape symbols, and
`H = 2 + 3*p + 3*b + 2*p*b` reachable white state shapes. A conservative bound
for this implementation is `O(H*(Q+Z)*N*M)` saturation work and
`O(H*N^2 + M)` storage, plus initialization. Type identifiers are hashed rather
than used as vector indices, so a large numeric ID does not allocate a large
array.

With neutral edges, these saturation bounds apply to `M' = |E'|`, not to the
original edge count. Closure uses per-source BFS, costing
`O(N*(N+E_epsilon))` work and up to quadratic storage. Terminal expansion visits
`sum_(u,a,v) |epsilon_predecessors(u)| * |epsilon_successors(v)|` combinations
before deduplication. No original-graph `O(N*M)` end-to-end claim is made for
this preprocessing.

## Build and run

In an already configured Lotus build, reconfigure with its usual dependencies
and options, then build:

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON
cmake --build build --target CanaryInterleavedDyckLCL \
  lotus-cfl-interleaved-dyck cfl_tests
ctest --test-dir build -R 'cfl_tests.InterleavedDyckLCL' --output-on-failure
```

Use `--help` for all CLI options. Queries print `may-reach` or `unreachable`;
sorted pair output is enabled by `--print-upper`. Successful analyses return
zero regardless of query outcome. Input errors and resource failures return
one and do not print a partially computed relation.

## Regression coverage

The shared test support contains 18 suites. It includes all 21,845 unary words
of length at most 7 and all 37,449 words with two types per alphabet of length
at most 5, compared with an independent two-stack word checker. These are
finite exhaustive tests, not a claim of exactness on arbitrary graphs.

Further checks cover crossing and nesting, typed mismatches, underflow,
neutral prefixes/suffixes/cycles, sparse and extreme identifiers, parallel
arcs, graph immutability, solver reuse, resource exceptions, a retained false
positive, and late white-to-gray promotions. Generated tests use fixed seeds:
300 DAGs against exact concrete two-stack search; 150 cyclic graphs against
bounded-depth concrete witnesses; 150 graphs with shuffled vertex and edge
insertion; and 100 graphs in all three modes against a separate synchronous
rule interpreter. The cyclic oracle is a witness underapproximation, not an
exact decision procedure. Long-word tests include nesting depth 96.

Eight CTest CLI regressions exercise parsing, directionality, baseline/refined
behavior, neutral paths, invalid queries, and limits. The shared test support
uses explicit exceptions for checks, not disabled-in-Release `assert` calls.
