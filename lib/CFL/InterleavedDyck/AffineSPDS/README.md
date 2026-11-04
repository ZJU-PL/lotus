# AffineSPDS: affine history synchronization for Lotus

This C++17 engine strengthens the endpoint-only SPDS upper bound by comparing
**joint affine relations between histories of projected witnesses**. It reuses the
existing `spds::PushdownSystem`, `postStar`, `preStar`, and `SaturationSession`
with an `AffineSemiring` weight domain.

The affine-history synchronization implemented here is an extension built on
top of the POPL 2019 construction, **not an algorithm from that paper**. The
paper supplies the separate
call/field PDS construction and weighted-saturation interface; the synchronized
history interpretation and its implementation are the extension.

## Semantics and scope

For a fixed shared map from original graph edges to GF(2) matrices, let
`rho(e1 ... ek) = M_e1 ... M_ek` and `rho(epsilon) = I`. For the call-valid and
field-valid trace languages of a query, the engine computes

```
H_call  = aff { rho(w) : w is a call-valid projected witness }
H_field = aff { rho(w) : w is a field-valid projected witness }
```

A pair is retained only if these affine spaces intersect. For empty endpoint
stacks, the following inclusions hold:

```
concrete same-path two-stack reachability
  <= joint affine history synchronization
  <= independent diagonal-block readout
  <= Boolean SPDS endpoint intersection.
```

The implementation computes exact affine hulls for the **fixed observer** on
each individual projection. It does not retain exact sets of matrices and does
not solve arbitrary same-path two-stack reachability exactly. Intersecting two
nonempty hulls may retain a false positive. Consequently, all APIs use `mayReach`
and candidate pair sets: false establishes unreachability under the supplied
graph semantics; true is not a concrete witness or a feasible program execution.

The library supports arbitrary directed graphs, typed labels, neutral edges,
cycles, sparse signed 64-bit vertices, maximum unsigned label IDs, isolated
vertices, and nonempty-stack queries. Stack and path lengths are not bounded.
The feature budget bounds the observation, not the execution.

## Build and integration

Library target: `CanaryInterleavedDyckAffineSPDS`.
CLI target: `lotus-cfl-interleaved-dyck affine-spds`.
Lotus GoogleTest suite: `cfl_tests` (cases `InterleavedDyckAffineSPDS.*`).
Namespace: `lotus::cfl::interleaved_dyck::affine`.

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON
cmake --build build --target CanaryInterleavedDyckAffineSPDS \
  lotus-cfl-interleaved-dyck cfl_tests
ctest --test-dir build -R 'cfl_tests.InterleavedDyckAffineSPDS' --output-on-failure
```

## Main API

```cpp
#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"
namespace dyck = lotus::cfl::interleaved_dyck;
namespace affine = dyck::affine;

int main() {
  dyck::Graph graph;
  graph.addEdge(0, 1, dyck::Label::openParenthesis(1));
  graph.addEdge(1, 2, dyck::Label::openBracket(2));
  graph.addEdge(2, 3, dyck::Label::closeParenthesis(1));
  graph.addEdge(3, 4, dyck::Label::closeBracket(2));

  affine::Solver solver;
  auto analysis = solver.prepare(graph);
  auto successors = analysis.analyzeFrom(0, affine::ComparisonMode::Joint);
  auto forward = analysis.queryFrom(0);
  auto backward = analysis.queryTo(4);
  const auto &comparison = forward.compare(4);

  // The positive answer in a general graph is only a candidate.
  return comparison.mayReach() && backward.mayReach(0) &&
         forward.mayAccept(2, {1}, {2}) ? 0 : 1;
}
```

`compare(v)` exposes both hulls, a verdict, and an optional separation equation.
`compareStacks(v, calls, fields)` queries precise stacks, top first. Forward
queries start with empty stacks at the source; backward queries fix empty stacks
at the target and ask about predecessor stacks. `Options::parentheses` and
`Options::brackets` select `spds::StackAcceptance::Any` for an existential stack
at the queried vertex instead of the default empty stack.

`analyzeAll(mode)`, `analyzeFrom`, and `analyzeTo` return the pair set selected
by `ComparisonMode::Joint`,
`Independent`, or `Projection`, plus saturation statistics. It computes only
the selected comparison. `analyzeDemands` groups requested pairs by source or
target and likewise computes only the selected mode. The graph-to-PDS conversion
is reused across queries, but saturation is still performed separately per
selected anchor.

Prepared analyses store dense edge endpoints and adjacency indexes. Directional
slices are cached by SCC and direction, bounded by `max_cached_slices` and
`max_cached_slice_rules` (either zero disables caching). Rules reuse original
edge weights and prepared multipliers; identity edges share one weight.
Demand slices intersect anchor reachability with reverse reachability from the
requested endpoints. All-pairs queries likewise avoid unrelated components.
Saturation still runs independently for each anchor; cached slices do not merge
the results or histories of different anchors.

`QueryResult` starts with individual readout and promotes to cached bulk endpoint
weights after `readout_batch_threshold` queries (default 8, zero disables automatic
promotion). `prepareReadout()` requests bulk evaluation explicitly. Full-endpoint
APIs batch immediately; sparse demand groups use individual queries. Explicit
stack queries and `SynchronizedResult` readouts have bounded 64-entry caches.
A single result is not safe for concurrent readout without caller synchronization.
Independent queries have separate algebra counters. Shared slice compilation
caches are protected by a mutex; saturation runs outside the lock.

## Fixed computational algebra

The matrix dimension is dynamic. Packed bit vectors and row-slice XOR implement
GF(2) arithmetic without a 32/64-bit dimension cap or an external algebra library.
Affine products consume and produce packed BitVector entries directly; Matrix
objects are materialized only at public representation boundaries.
There are tests crossing word boundaries at dimensions 9, 63, 64, and 65.
Bit vectors of up to four machine words are stored inline, covering the default
automatic observer without per-vector heap allocation. Identity observers use
the Boolean prepared analysis for all-pairs and batch scopes; this is exact
because every reachable affine history is the singleton identity matrix.
Affine rules for identity observers are created lazily only when an API requests
affine histories; Boolean-compatible analysis does not build unused weighted PDSs.
Affine bases keep their first two directions inline and share immutable storage
across weight snapshots; mutation detaches on demand. Product candidates are
inserted as a stream and reduced in one batch, so a full-rank result stops early.
Existing transition updates fuse affine product generation with basis insertion,
avoiding a separately canonicalized temporary product. Non-identity singleton
rule weights also cache their packed right-multiplication rows during PDS setup.
After a transition's first full visit, saturation propagates only newly added
basis pivots through epsilon, rule, and pre* push joins. Pending pivots use a
lazy paged arena, so transitions whose weight never grows pay no per-edge delta
allocation. Full propagation remains a separate fast path.
Bulk weighted readout also propagates basis deltas and fuses product generation
with insertion. Delta products terminate when the destination becomes full,
including before triple-product fallbacks. Right-direction multipliers are reused
across left input deltas.

Block-diagonal observers use concatenated block entries with a single JOINT basis.
For the largest default observer this reduces 196 coordinates to 34, retaining
cross-block correlations. Every assigned matrix is checked before compression;
custom matrices with cross-block entries use the dense layout. Set
`Options::compress_observer=false` for an exact dense-layout comparison.
Products operate blockwise, while `Matrix` objects and certificates remain dense
public matrices. `AffineSpace::coordinates()` reports the stored coordinate count;
`offset()` and `directions()` use that layout. Use `decodeEntries(bits)` to turn
an encoded point/direction into a dense matrix. Affine-space equality and
intersection support comparison with a dense representation of the same hull.

Intersection tests first recognize shared offsets and full spaces, then use
echelon elimination without unnecessary RREF maintenance. A negative
`HistoryComparison` retains its separating equation from that same elimination,
so a later `certificate()` call does not factor the hulls again.

An affine space is either empty or `a + span(B)`. `B` is a canonical reduced
row-echelon basis; `a` is reduced by that basis. Equality is semantic, so redundant
derivations do not keep a worklist alive and insertion order does not affect the
result.

The semiring operations are:

```
zero         = the empty affine space
one          = { I }
combine(U,V) = aff(U union V)
extend(U,V)  = aff { X*Y : X in U, Y in V }
```

A singleton containing the all-zero MATRIX is **not** semiring zero. Alternative
paths are combined with affine hull, never XOR-summed: XOR summation would
incorrectly cancel real witnesses.

For `U = a + span(u_i)` and `V = b + span(v_j)`, multiplication computes

```
ab + span( u_i*b, a*v_j, u_i*v_j ).
```

The bilinear terms are required; points are never enumerated. For matrix
dimension `r`, the dense ambient dimension is `D=r*r`; a block layout uses
`D=sum(block_dimension^2)`. Affine rank and strict
ascending chains are finite. Multiplying ranks `h` and `k` produces at most
`1+h+k+h*k` vectors before basis reduction.

## How matrices are chosen

`HistoryObserver::set(edge, matrix)` accepts arbitrary square GF(2) matrices;
both projections use the same matrix for an original edge.

The default **deterministic graph-only heuristic** works as follows:

1. Sort original edges by `(source, target, label kind, label id)`.
2. Select one non-default outgoing edge at each branch, then additional branch
   alternatives, then remaining sorted edges, up to `max_events` (default 4).
3. Construct a 2x2 parity observer for each selected edge and 3x3 ordered-pair
   observers for the first selected pairs, up to `max_order_pairs` (default 2).
4. Form their direct sum and saturate the **joint** affine hull. Default matrix
   dimension is at most 14. No selection yields the 1x1 identity observer.

Increasing the budgets retains earlier component observations. This is an
experimental graph-only default, not a claim that the selected events suffice
for every client.

Additional deterministic constructors:

- `parity(events)`: one bit toggled for each event in the set.
- `orderedPair(first, second)`: counts first events, second events, and ordered
  first-before-second subsequences modulo two. Events can belong to both sets;
  one event is not counted as preceding itself.
- `cyclic(events, modulus)`: a permutation-matrix observer, including modulo-three
  distinctions not expressible by the earlier truncated unary parity hierarchy.
- `directSum(observers)`: one joint observation with component boundaries.

These are constructors within one algebra. Unmapped edges carry identity.

## Joint versus independent synchronization

`correlation.dot` contains two binary choices, x and y. The call projection
accepts exactly x=y, whereas the field projection accepts exactly x!=y. Every
edge is supported by both projected queries, but there is no common balanced
path. The supplied observer tracks x and y in two parity blocks.

```sh
FIXTURES=tests/regress/CFL/InterleavedDyck/AffineSPDS
build/bin/lotus-cfl-interleaved-dyck affine-spds \
  --query 0 14 --observer "$FIXTURES/correlation.observer" \
  --certificate "$FIXTURES/correlation.dot"
```

The result includes:

```
query: unreachable
reason: affine-separated
certificate-functional: 0100/0000/0001/0000
certificate-call-value: 0
certificate-field-value: 1
certificate-verified-against-hulls: true
```

`--mode independent` selects the weaker per-block readout and retains this pair.
`--mode spds` selects endpoint nonemptiness. **Both modes still compute the same
joint weighted saturation**; they are precision ablations, not independent
performance implementations. For a Boolean performance baseline, use the
original SPDS executable, not `--mode spds`. `--identity` is another semantic
baseline, with a trivial observer and very small domain overhead.

For non-block-diagonal custom matrices, independent mode projects the full hull
onto declared diagonal blocks; it need not equal a separately composed observer.

## Certificates

When both hulls are nonempty and disjoint, `separate(left,right)` produces a
`SeparationCertificate { functional, left_value, right_value }`. The functional
annihilates both direction spaces, and its evaluations on their offsets differ.
The implementation derives it directly from the reduced basis of the union of
directions and the reduced offset difference.

`certificate.verify(left,right)` checks all these conditions. CLI output can
include the certificate in text or JSON.

**Certificate boundary:** this verifies separation against the supplied affine
hulls. It is not an independent proof that the hulls cover all pushdown paths.
The saturation implementation and event mapping remain part of the trusted
computation. Empty-projection rejection needs no separating functional, and
reports `ProjectionRejected` with no certificate. A certificate is not returned
for overlapping hulls.

## Observer file format and CLI

```
# Comments begin with #. Missing original edges use identity.
dimension 2
block 0 2
edge 0 1 op--1 11/01
```

Each edge directive gives source, target, a Core label, and slash-separated bit
rows. Optional block directives must partition the diagonal in order. Unknown
edges, duplicate edge assignments, invalid dimensions/rows/IDs, and incomplete
block partitions are rejected. `--dump-observer` preserves the matrix map and
block metadata, enabling reproducible experiments.

`--json` emits numeric statistics, the selected result, optional sorted pairs,
and optional joint certificates. Query scope is selected with `--all-pairs`,
`--source`, `--target`, `--query`, or `--queries FILE`; `--direction` selects
post*, pre*, or automatic batch grouping. A scope is required so large graphs
cannot accidentally start an all-pairs run. Explicit stack flags and prefix
flags are listed by `--help`.

The shared serial corpus runner and manifest are documented in the SPDS README.
Select this engine with `--engine affine`; phase counters include affine
saturation and weighted readout. Statistics also expose the stored coordinate
dimension, new prepared weights, compiled rules, slice-cache hits, matrix products,
basis reductions/insertions, and copy-on-write detachments. Algebra counters belong
to one query and include its readout and comparisons. `--timings` additionally
reports observer construction, intersection and certificate microseconds; these
comparison times are separate from automaton readout. Bulk statistics are captured
after comparisons, and the reported maximum rank includes queried readout hulls.

The Core DOT parser currently reads edge statements, not isolated vertex
statements. Use `--vertex V` or `Graph::addVertex(V)` to retain isolated vertices.

## Front-end-neutral synchronized data-flow builder

`AffineSPDS/Synchronized.h` provides `affine::SynchronizedSystem`. It follows the
`SPDS` variable/statement encoding, but carries history weights on BOTH
systems and compares their hulls at synchronized configurations:

```cpp
#include "CFL/InterleavedDyck/AffineSPDS/Synchronized.h"
namespace a = lotus::cfl::interleaved_dyck::affine;

a::SynchronizedSystem system(2);
a::Matrix event = a::Matrix::parse("11/01");
system.addCall({0,10}, {1,20}, 11, event);
system.addStore({1,20}, {2,21}, 7);
system.addReturn({2,21}, {3,11});
system.addLoad({3,11}, {4,12}, 7);
auto result = system.postStar({{0,10}, {}, {}});
bool candidate = result.mayAccept({{4,12}, {}, {}});
```

`addNormal`, `addStore`, `addLoad`, `addCall`, and `addReturn` take an optional
matrix (identity when omitted). A transfer's matrix is attached to every
corresponding rule in both projections exactly once. `compareAt` queries an
exact access path in any calling context; `compareNode` permits any access path
and context. `preStar` is also available.

Clients must supply data-flow rules, globally scoped variable identities,
actual/formal mappings, kills, and alias-induced transfers. This is not a new
LLVM/Soot front end, automatic alias solver, or strong-update analysis. These
history matrices are not protocol/typestate weights: property-coupled typestate
and observer synthesis are deliberately outside this implementation.

## Generic and incremental PDS access

`AffineSemiring` satisfies the SPDS weighted-domain interface and works with
arbitrary PDS rules and regular seeds. `SaturationSession<AffineSemiring>` also
supports monotone `addRule(...)` followed by `run()`; controls and the observer
remain fixed, and rule deletion is unsupported. See the SPDS README for the
generic session and regular-language APIs.

`affine::synchronize` combines queries to two affine-weighted PDS automata. It
checks dimensions and direction, but cannot verify that a client assigned the
same event semantics to its two rule systems. Use the graph solver or the
synchronized builder when that shared mapping should be enforced structurally.

## Limits and validation

`Options::limits` applies to each individual PDS saturation. Optional limits
cover states, transitions, and weight promotions; `max_matrix_dimension` guards
the accepted observer dimension before graph saturation. For automatic observers,
the dimension is checked before allocating direct-sum matrices. Allocation/resource
failures throw exceptions. No rank truncation, widening, partial saturation, or
resource exhaustion is silently interpreted as a sound negative result.
The dimension check is not a global memory budget and cannot undo allocation of
a custom observer already built by the caller; choose modest feature budgets.

Readout over a saturated automaton is finite-height too, but its extra work is
not counted against the saturation's update limit. There is no whole-analysis
wall-clock or memory-budget controller.

The CLI buffers result output until the complete requested operation succeeds.
Exit codes are 0 (complete), 2 (invalid input/I/O), and 3 (resource failure).

Tests cover algebraic laws, exact small oracles, cyclic/recursive cases,
post*/pre*, incremental updates, observer round trips, wide matrices, and the
precision hierarchy.

## References / attribution

- J. Späth, K. Ali, E. Bodden. Context-, Flow-, and Field-Sensitive Data-Flow
  Analysis using Synchronized Pushdown Systems. POPL 2019, article 48.
  DOI: 10.1145/3290361. In particular Definition 4, Section 4.1, and Sections 5.2–5.3.
- The affine-set-of-matrices representation is related to the established
  interprocedural affine-relation/MOS domain. This implementation does not claim
  to invent affine matrix domains or their semiring properties.
