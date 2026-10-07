# Subcubic PEG alias analysis

This engine implements Qirun Zhang, Xiao Xiao, Charles Zhang, Hao Yuan, and
Zhendong Su, **Efficient Subcubic Alias Analysis for C**, OOPSLA 2014,
[DOI 10.1145/2660193.2660213](https://doi.org/10.1145/2660193.2660213).

The implementation includes Algorithm 1's two-phase memory-alias propagation,
Algorithm 2's value-alias computation, Section 3.4's fast-set differences, and
optional connected-component decomposition. Its input is a pointer expression
graph (PEG), and its output is the exact memory (`M`) and value (`V`) alias
relations of the paper's grammar. Both relations are symmetric; neither is
transitive.

## Files and integration

- `SubcubicAA.h` / `SubcubicAA.cpp`: C++17 core API and the specialized engine.
- `SubcubicAAAdapter.h` / `SubcubicAAAdapter.cpp`: conversion from Lotus
  `LabeledGraph`, with label and reverse-edge validation.
- `lotus-cfl-subcubic-aa`: dedicated graph-input command-line driver, linked
  against `CanaryClassicalCFL`.
- `tests/unit/CFL/Classical/SubcubicAATest.cpp`: paper examples, boundary cases,
  and differential comparison with the paper's grammar in `SolverSession`.

The API is in `lotus::cfl::classical::engines::subcubic`. It is separate from
the arbitrary-grammar `SolverSession` backend interface. Read
[`docs/source/cfl/classical/subcubic_aa.rst`](../../../../../../docs/source/cfl/classical/subcubic_aa.rst)
for the algorithm mapping, interface, and build instructions.

## Input contract

`Problem` uses dense vertex IDs and two forward edge lists: `assignments`
(`a`, from the right-hand side to the assigned expression) and `dereferences`
(`d`, from `e` to `*e`). Reverse edges are implicit. Every assignment target
must have an incoming dereference. The core rejects invalid endpoints and
graphs violating this PEG condition. Duplicate edges are harmless.

The graph adapter accepts only `a`, `abar`, `d`, and `dbar`, and requires the
corresponding reverse edge for every edge. Use the explicit `--bidirectional`
flag for files that omit reverse edges. Field labels such as `f_0`, raw
load/store constraints, and arbitrary CFL grammars are outside this engine's
input language. Lotus's standard `cfl-peg` alias client supports additional
labels and graph reductions; its outputs must meet this narrower contract
before they can be passed to this engine.

## Run the paper example

```sh
build/bin/lotus-cfl-subcubic-aa \
  tests/regress/CFL/Classical/subcubic-aa-figure4.graph \
  --query 'a,*d' --json-stats

build/bin/lotus-cfl-subcubic-aa \
  tests/regress/CFL/Classical/subcubic-aa-figure4.graph \
  --memory-only --query 'b,*c' --print-aliases
```

Figure 4 has 7 ordered memory-alias pairs and 30 ordered value-alias pairs,
including diagonal pairs. It demonstrates `V(a,*d)` and `V(*d,e)` while
`V(a,e)` is false.

`--memory-only` stops after Algorithm 1 and selects memory queries by default.
`--no-fast-sets` uses scalar membership checks over the same packed storage;
`--no-components` disables decomposition. With `--json-stats`, statistics,
queries, and any requested pair enumeration form one JSON object.

## Correctness and performance boundaries

Algorithm 2 retains an immutable set of the initial `V2` seeds for its final
join. Newly produced pairs do not become join inputs; doing so would
incorrectly introduce transitive value aliases. Nullable `dbar V d` seeds
include all pairs of dereference successors of a common source, which also
handles PEGs with multiple dereference successors exactly.

Fast sets use 64-bit words and enumerate only bits absent from the destination
relation. The paper's `O(n^3 / log n)` bound assumes a RAM with word size
`Theta(log n)`. This implementation supplies the corresponding word-parallel
operations with a fixed machine word size; it does not claim a new measured
asymptotic or reproduce the paper's benchmark timings. Results retain packed,
component-local relations, and printing all pairs adds output-proportional
work.
