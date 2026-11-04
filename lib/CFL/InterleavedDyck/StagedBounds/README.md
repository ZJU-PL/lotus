# Interleaved-Dyck Staged Bounds

> **This module is an approximation pipeline.** It does not compute exact
> general typed interleaved-Dyck reachability.

This directory contains a native C++17 implementation of the staged algorithm
from the artifact **A Better Approximation for Interleaved Dyck Reachability (SOAP'24)**
by Giovanna Kobus Conrado and Andreas Pavlogiannis.

The port retains the artifact's main stages:

1. regular-language filtering through a graph/automaton product;
2. intersection of independently witnessed parenthesis- and bracket-Dyck
   reachability;
3. a sound Dyck-over-the-union underapproximation;
4. edge-tracing mutual refinement;
5. the stronger parity/endpoint grammar; and
6. pairwise on-demand refinement.

The `CnfGrammar`, `CnfGraph`, and `CnfTypes` implementation supplies the SAS'23
CNF saturation and derivation-tracing engine. Public graph, DOT parser,
individual approximation, and full-pipeline APIs are split between
`include/CFL/InterleavedDyck/Core/Graph.h` and
`include/CFL/InterleavedDyck/StagedBounds/Solver.h`.

## Guarantees and result interpretation

Let `R` be the true typed interleaved-Dyck relation, `L` the
`underapproximation` result, and `U` any sound projected/refined upper bound.
The intended invariant is:

```text
L  subset-of  R  subset-of  U
```

The public `ApproximationResult` should be read as follows:

| Result field | Kind | Interpretation |
|---|---|---|
| `underapproximation` | Lower bound | Present means definitely reachable |
| `intersection` | Upper bound | Projections accept, possibly via different paths |
| `mutual_refinement` | Tighter upper bound | Classic derivation tracing retained the candidate |
| `stronger_grammar` | Tighter upper bound | Parity/endpoint refinement retained the candidate |
| `on_demand` | Final upper bound | Pairwise refinement retained the candidate |

A pair in the lower bound is certified reachable. A pair absent from the final
upper bound is certified unreachable relative to the modeled graph. A pair in
the final upper bound but outside the lower bound remains unknown. The pipeline
is exact on a particular input only when its lower and final upper bounds
coincide.

The reference artifact did not include a license file in the supplied source
tree. This port is therefore an independent C++ reimplementation based on the
published algorithm and observable artifact behavior, rather than a verbatim
copy of its Go/Python sources. Its DOT benchmark inputs are stored under
`benchmarks/real-world/CFL/InterleavedDyck/` with provenance noted there.

## Command line

```sh
cmake --build build --target lotus-cfl-interleaved-dyck
build/bin/lotus-cfl-interleaved-dyck staged-bounds \
  --method mutual-refinement \
  benchmarks/real-world/CFL/InterleavedDyck/taint/faketaobao.dot
```

`--method` selects `regularization`, `intersection`, `underapproximation`,
`mutual-refinement`, `stronger-grammar`, `on-demand`, or `all`. Use
`--analysis taint|value-flow` to select the client analysis (default
`taint`), `--print-lower` for certified reachable pairs, or `--print-result`
for the selected method's pairs. The tool never adds reverse arcs.

Use `--factorized-tracing` to reconstruct the union of contributing terminal
edges without eager derivation records. Saturation and reconstruction share
symbol-indexed, adaptive sparse/bitmap relations. Taint regularization constructs
only product states that can participate in accepted paths. `--stage-stats`
reports stage wall time and cumulative process peak RSS on stderr; the RSS high-water mark
never resets between stages. In particular, `--method mutual-refinement` still
runs regularization, intersection, and underapproximation first.

See [the factorized-tracing algorithm](FACTORIZED_TRACING.md) for relation
representation, saturation and reconstruction, correctness arguments, product
trimming, and complexity bounds.

## Boundary with MutualRefinement

`StagedBounds` is the domain-facing pipeline; its `mutual_refinement` namespace
contains the low-level CNF engine used by several stages.

| Responsibility | `StagedBounds` | `MutualRefinement` |
|---|---|---|
| Input model | Shared typed `op/cp/ob/cb/normal` graph | Integer-encoded `CnfGraph` and `CnfGrammar` |
| Grammar ownership | Builds classic, union-Dyck, parity, and endpoint grammars | Stores and saturates a supplied CNF grammar |
| Pipeline policy | Regularization, lower bound, condensation, refinement, on-demand checks | CFL closure, derivation records, contributing-edge closure |
| Result semantics | Named lower and upper bounds for interleaved Dyck | Grammar-relative reachability edges and traces |
| Benchmark knowledge | Taint and value-flow modes | None |

The single `CanaryInterleavedDyckStagedBounds` library links to
`CanaryInterleavedDyckCore`. The solver translates shared typed labels into the
engine's integer grammar/edge representation, invokes CFL saturation with
tracing, and interprets the trace as an interleaved-Dyck refinement. The nested
engine does not parse typed labels, choose an approximation grammar, compute
lower/upper bounds, or run the staged pipeline.

## Relationship to other Lotus CFL components

This module is currently the concrete bridge among Lotus's related
interleaved-Dyck implementations:

- its `mutual_refinement` engine provides CNF saturation and derivation
  tracing;
- [`InterleavedDyckGraphReduction`](../GraphReduction/README.md) offers a separate
  file-oriented graph-simplification approach. Its output can be reparsed by
  the shared core, but this pipeline does not invoke it automatically; and
- [`MCFL`](../MCFL/README.md) offers the complementary `G_d^circ`/`G_d^+`
  underapproximation hierarchy, a general MCFG solver, and the exact adaptive
  solver for the narrower bidirected unary problem. It uses independent graph,
  grammar, and result representations.

The conceptual overlap does not imply API compatibility. A future unified
pipeline would need explicit graph/label conversion and preservation contracts
for every preprocessing or refinement boundary. See the
[CFL overview](../README.md) for the side-by-side comparison.
