# Interleaved-Dyck Reachability

This directory is the umbrella for Lotus's interleaved-Dyck implementations.
The same structure is mirrored by `include/CFL/InterleavedDyck`.

| Subdirectory | Role | Guarantee or result |
|---|---|---|
| [`Core`](Core/README.md) | Typed labels, graphs, DOT parsing, unary projection, and shared bidirected-Dyck support | Representation and common algorithms |
| [`Unary`](Unary/README.md) | Adaptive and fixed-counter algorithms for bidirected unary `D1`-interleaved-`D1` | Exact component partition after unary projection |
| [`StagedBounds`](StagedBounds/README.md) | Projected languages, union-Dyck lower bounds, parity refinement, and on-demand checks | Certified lower bound and progressively tighter upper bounds |
| [`LCL`](LCL/README.md) | POPL 2017 linear-conjunctive-language saturation with gray-node refinement | Sound upper bound for directed, typed interleaved Dyck |
| [`ILP`](ILP/README.md) | POPL 2023 single-source/single-target path expressions and integer parenthesis counting | Demand-driven InterDyck over-approximation |
| [`MCFL`](MCFL/README.md) | Normal-form MCFG solver and the dimension-indexed `G_d` hierarchy | Exact for a supplied MCFG; underapproximation for generated Interleaved-Dyck grammars |
| [`GraphReduction`](GraphReduction/README.md) | PLDI 2020 file-oriented graph simplification | Reduced graph preserving the reduction's reachability property |

## Public namespaces

```text
lotus::cfl::interleaved_dyck
├── unary
├── spds
├── affine_spds
├── staged_bounds
├── lcl
├── ilp
└── mcfl
```

`Core` owns the types directly in `lotus::cfl::interleaved_dyck`; the other
public modules use nested namespaces matching their directories.

The SAS'23 mutual-refinement CNF engine is internal to `StagedBounds`, under
`staged_bounds::mutual_refinement`; it is not a sibling analysis.

## Choosing an analysis

| Question | API |
|---|---|
| Exact bidirected unary reachability | `interleaved_dyck::unary::AdaptiveSolver` |
| POPL 2022 fixed-counter baseline | `interleaved_dyck::unary::FixedCounterSolver` |
| Typed lower and upper bounds | `interleaved_dyck::staged_bounds::Solver` |
| POPL 2017 typed upper bound | `interleaved_dyck::lcl::Solver` |
| POPL 2023 single-pair ILP upper bound | `interleaved_dyck::ilp::Solver` |
| Dimension-indexed certified pairs | `interleaved_dyck::mcfl::InterleavedDyckSolver` |
| Exact reachability for a client-supplied MCFG | `interleaved_dyck::mcfl::Solver` |

`Core`, `StagedBounds`, `LCL`, and `MCFL` preserve the directed arcs supplied by the
input. `Unary` rejects non-bidirected input by default; explicit
symmetrization changes its interpretation to an overapproximation of the
original directed graph. `GraphReduction` uses private synthetic orientations
inside its reduction and does not expose them as original input arcs.

## Synchronized pushdown systems (POPL 2019)

[`SPDS`](SPDS/README.md) provides `interleaved_dyck::spds::Solver`, a directed,
typed upper-bound engine based on independent call/field pushdown closures.
It also exposes regular-language post*/pre*, incremental weighted saturation,
and a variable-at-statement data-flow builder. Its namespace is `spds` and its
library target is `CanaryInterleavedDyckSPDS`. Positive pairs are candidates,
not same-path witnesses. This engine neither changes Core nor depends on LCL.
