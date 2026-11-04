# Exact Unary Interleaved Dyck Reachability

`CanaryInterleavedDyckUnary` contains exact algorithms for bidirected unary
`D1`-interleaved-`D1` reachability. The directory names the problem scope;
algorithm names live below that scope.

| API | Algorithm | Finite control after preprocessing |
|---|---|---|
| `AdaptiveSolver` | Adaptive two-arm construction | `O(n^2)` states |
| `FixedCounterSolver` | Kjelstrøm–Pavlogiannis POPL 2022, Algorithm 1 | `O(n^3)` states |

Both algorithms consume `interleaved_dyck::Graph` and share unary projection,
input validation, quotient sparsification, and the bidirected one-counter
component backend from `CFL/InterleavedDyck/Core`. This makes their comparison
about finite-control construction rather than parser or preprocessing choices.

## Exactness boundary

Exactness requires:

- every projected arc has its complement-labeled reverse arc;
- every parenthesis ID operates on counter 1 and every bracket ID on counter 2;
- queries ask for balanced paths between zero-counter configurations.

The module does not claim exactness for multi-type
`D_k`-interleaved-`D_k` reachability.

Both option types expose `input_policy`:

- `RequireBidirected` is the default. It rejects a missing complement reverse
  arc and remains exact for the supplied graph.
- `AddMissingReverseEdges` explicitly symmetrizes the graph. The result is
  exact for the symmetrized graph and a sound overapproximation for the
  original directed graph. Result statistics record the inserted arcs and
  weaker guarantee.

The shared parser never adds reverse arcs implicitly.

## Algorithms

### Adaptive

`AdaptiveSolver::solveShallow(graph, K)` computes exactly the
zero-configuration partition inside
`X_K = {(v,a,b) : min(a,b) <= K}`. It constructs two finite-control
one-counter arms, computes their zero-height components, derives positive
height labels through a parent map, and merges both arm partitions at their
boundary.

`AdaptiveSolver::solve(graph)` first applies the fixed-alphabet quotient,
splits the quotient into weak components, chooses `K = 6 * n` separately for
each component of size `n`, and lifts the resulting partition to the input
vertices. Components are processed sequentially. Set `AdaptiveOptions::sparsify`
to `false` to skip the quotient; decomposition and fast paths still apply.
`solveShallow` also decomposes the graph but preserves the caller's K in every
component and never applies the full-reachability quotient.

### Fixed counter (POPL 2022)

For each mixed-counter weak component with `n` vertices, `FixedCounterSolver` uses

```text
C = 18*n^2 + 6*n.
```

It constructs states `(v,j)` for every `0 <= j <= C`, storing counter 2 in
`j`. Counter-2 labels become epsilon transitions that change `j`; counter-1
labels remain the opening/closing labels of a bidirected one-counter graph.
Vertices `u` and `v` are connected exactly when `(u,0)` and `(v,0)` are in the
same zero-height component.

Both solvers skip singleton components and solve single-counter components
directly. The latter optimization is also exact for every shallow K because
the unused counter stays zero.

The shared backend consumes generated transitions without building expanded
edge vectors. Epsilon connections are emitted once as undirected connections
and contracted by union-find before allocating closing-edge tables. These
tables use dense epsilon-component IDs; repeated adjacent targets in each
source/label list share one pool entry. Final Dyck IDs are dense as well.
This changes representation and scheduling, not reachability. Disabling
sparsification retains the local fixed-counter construction for mixed components.

Adaptive finishes vertical labeling and sorting before constructing the
horizontal arm. Sort scratch is released before allocating the merge DSU,
parent/representative tables use compact component counts, and only the queried
zero-state roots receive final identifiers. FixedCounter likewise avoids a
full lifted-state identifiers array. Result queries retain only the input
vertex partition.

### Complexity and measurements

Sparsification is preprocessing. For a unary input with `N` vertices and `M`
arcs, it takes `O((N+M) alpha(N))` amortized time and `O(N+M)` space for the
fixed alphabet of two counters. Unary projection before it uses hash-based
deduplication and has expected `O(N+M)` time. The quotient has `q <= N` vertices
and at most `4q` arcs: at most one closing target per component per counter,
plus each closing arc's opening reverse. Epsilon arcs are contracted away.

The implementation consumes generated input edges, allocates closing tables
after epsilon contraction, reuses the backend's dense component mapping, and
exports its final functional closing lists directly. It does not renumber an
already-dense map or rescan the original graph to construct quotient edges.
Adaptive also reuses this exported summary for its vertical parent map.
These changes reduce memory traffic and constants, not the inverse-Ackermann
factor. Reading the input already requires `Omega(N+M)` work.

With quotient component sizes `n_i`, the default Adaptive construction uses
`O(sum_i n_i^2)` finite control and near-quadratic time with union-find factors,
in addition to preprocessing and lifting. Its sequential construction workspace
depends on the largest component. FixedCounter's mixed-component construction
remains `O(sum_i n_i^3)` states; streaming does not remove that exponent.

For an unquotiented graph with `n` vertices, `m` arcs and caller-supplied K,
Adaptive's construction costs `O((n+m)(K+1))` up to union-find factors.
`--direct` is therefore not a near-quadratic guarantee on dense graphs.

`stats().execution` reports projection, preprocessing, decomposition, solving,
lifting, and total microseconds, component sizes and fast-path counts.
Adaptive additionally reports vertical, horizontal and merge microseconds.
Dyck statistics distinguish generated closing edges, stored pool entries and
scanned entries, and report epsilon contraction, ingestion and saturation time.
Construction counts sum actual mixed-component work; threshold/bound fields
report the maximum used (shallow queries report the requested K).

`peak_working_bytes` estimates construction container payload, excluding input
and component graphs, final output maps, allocator overhead and temporary
reallocation peaks. With `--stats`, macOS/Linux also report process peak RSS,
which includes parsing and allocator retention. This is a process high-water
mark, not a per-phase memory measurement.

## C++ API

```cpp
#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/Unary/Solver.h"

using namespace lotus::cfl;

interleaved_dyck::Graph graph =
    interleaved_dyck::Graph::parseDotFile("bidirected.dot");
auto adaptive = interleaved_dyck::unary::AdaptiveSolver{}.solve(graph);
auto fixed = interleaved_dyck::unary::FixedCounterSolver{}.solve(graph);
```

## Command line

One tool selects either algorithm:

```sh
cmake --build build --target lotus-cfl-interleaved-dyck
build/bin/lotus-cfl-interleaved-dyck unary \
  --algorithm adaptive --stats bidirected.dot
build/bin/lotus-cfl-interleaved-dyck unary \
  --algorithm fixed-counter --stats bidirected.dot
```

`--direct` disables quotient sparsification. `--shallow K` is specific to the
adaptive algorithm. `--phase-timing` requires `--stats` and selects the
compile-time-instrumented Adaptive implementation used by the RQ2.2 phase
breakdown. Without it, the additional fine-grained timers are compiled out.
`--print-pairs` groups vertices by component and streams
non-reflexive pairs in `O(|V| + output)` time without storing a pair set.
`--bidirect` explicitly selects `AddMissingReverseEdges`; output always states
the selected algorithm and exactness guarantee.

## Benchmark use

The algorithms can parse the datasets under
`benchmarks/real-world/CFL/InterleavedDyck`, but that published corpus is
directed. The default policy therefore rejects it. Experiments may explicitly
use `--bidirect` as a sound overapproximation, but must report that graph
transformation, or use a genuinely bidirected corpus for exact comparisons.

## Validation and fidelity

Tests cover shallow thresholds, repeated arm switching, cascading component
merges, random and exhaustive small graphs, exact fixed-counter construction
sizes, quotient equivalence, and directed-input policies.

`FixedCounterSolver` faithfully implements the exact bounded-path construction
of POPL 2022 Algorithm 1 and the ordinary-Dyck sparsity reduction. The paper's
experimental doubly-self-looped-node removal and motif trimming are not
included. Controlled comparisons deliberately give both algorithms the same
shared preprocessing; reproducing the artifact's wall-clock table would
require exposing those heuristics as a separate experimental configuration.
