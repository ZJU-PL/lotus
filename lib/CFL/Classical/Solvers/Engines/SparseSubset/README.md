# Sparse-subset CFL saturation

This engine implements the all-pairs saturation construction from
“Beating Rytter’s Bound: All-Pairs CFL Reachability in
O(n^3 (log log n)^2 / (log n)^2) Time”.

## Mapping to the paper

* `BitPlaneCounters` stores exact unsigned lane counters in bit planes and
  implements lane-wise vector addition using bit-sliced full-adder carry
  propagation. This is used both for sparse-subset vectors and the two
  orientation-specific intersection-count arrays.
* `SparseSubsetTable` stores `V_S` for every nonempty subset of at most `s`
  coordinates in each length-`L` block. Coordinate incidence lists update
  every stored subset vector containing a newly set matrix entry. A query
  partitions its sorted coordinate batch at block boundaries and into pieces
  of at most `s`, then adds the corresponding precomputed vectors.
* `TwoSidedSetPair` maintains the two growing families, their snapshots and
  pending lists, the sparse tables for both snapshot matrices, `c_X`,
  `I_col`, and `I_row`. It implements both promotion directions and the
  exact pending-list corrections in `Report` and `Count`. Groups are aligned
  to sparse-table blocks using the padded-universe convention from the paper.
* `engines::sparse_subset::solveAllPairs` creates row and column set-pair
  instances for every binary production. New facts are forwarded to the
  affected instances at insertion time. Processing a `B(u,v)` fact reports
  `R_C[v,:] \\ R_A[u,:]`; processing a `C(v,x)` fact reports
  `R_B[:,v] \\ R_A[:,x]`. Reports complete before their returned facts are
  inserted into the relation.
* `SolverSession.cpp` exposes the engine through
  `parseSolverBackend("sparse-subset")` and `solverBackendName`, without
  widening `CFL/Classical/Solvers/SolverSession.h`. A session recomputes the
  all-pairs fixed point after each graph update.

## Layout

Following the rest of the Classical CFL tree, public engine headers live
under `include/` and only implementation files stay under `lib/`:

* `include/CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetCFL.h`
  — backend entry point.
* `include/CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetTabulation.h`
  — `BitPlaneCounters`, `SparseSubsetTable`, and `TwoSidedSetPair`.
* `lib/CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetCFL.cpp`
  — `solveAllPairs` implementation.
* `tests/unit/CFL/Classical/SparseSubsetEngineTest.cpp` — differential
  coverage for the counters, sparse tables, set-pairs, and the solver
  against `sparse-bitvector`; `tests/regress/CFL/Classical` runs the CLI
  drivers with `--solver sparse-subset`.

## Parameters and implementation notes

For a caller-supplied `epsilon` in `(0,1)`, the engine uses
`D=ceil(n/lambda^2)`, `g=lambda`, `eta=epsilon/4`,
`s=max(1,floor(eta log_2(n)/log_2(e ceil(n/D))))`, and
`L=s ceil(n/D)`, where `lambda=ceil(log_2(n))`. The internal universe is
padded to a multiple of `gL`, so every sparse-table block belongs to exactly
one report group.

The C++ representation uses `std::map` for deterministic subset lookup and
explicit bit-plane vectors. It realizes the paper's exact counting and
promotion/report invariants, but the asymptotic theorem is a word-RAM analysis:
allocator, map lookup, and cache costs are not captured by that model. The
solver backend currently uses `epsilon=0.25` through `SolverSession`; direct
callers of `solveAllPairs` can select another fixed epsilon.

The algorithm returns every terminal and nonterminal relation. It is an
all-pairs CFL solver; it does not substitute a designated-pair-only
approximation.
