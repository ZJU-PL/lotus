# Classical CFL Reachability Solvers

This directory hosts the solving engines and preprocessing passes of
`lib/CFL/Classical`; the implementation mirrors
`include/CFL/Classical/Solvers`.

- `ConstraintGrounding.cpp` — structural set-variable grounding analysis
  (separate from the CFL node-pair relation solvers).
- `SolverSession.cpp` — public backend selection, incremental session state,
  and solver orchestration.
- `Engines/` — the CFL-reachability solving algorithms.
- `Preprocessing/` — graph simplification and RSM-guided foldability.

## Engines

Each engine derives the same exact grammar-relative relation from the input
graph; the differences are algorithmic.

| Engine | Idea | Paper |
|---|---|---|
| `TransitiveClosure` | Generic incremental transitive closure over forward/reverse sparse bitvectors; every production `X -> X X` closes with a dedicated incremental bitvector closure. | Classical algorithm; no specific paper. |
| `CAT` | Context-aware tabulation: per-symbol usage contexts prune redundant derivation work; guarded, language-preserving transitivity rewrites. | Chenghang Shi et al., ICSE 2026, DOI [10.1145/3744916.3773222](https://doi.org/10.1145/3744916.3773222). |
| `IEOCE` | Iterative-epoch online cycle elimination: collapses SCCs during solving; the `IeaOcr` variant maintains a minimum-equivalent graph. | Pei Xu et al., "Iterative-Epoch Online Cycle Elimination for Context-Free Language Reachability", OOPSLA 2024, DOI [10.1145/3649862](https://doi.org/10.1145/3649862). |
| `PEARL` | Multi-derivation: batch-propagates reachability over transitivity-aware subgraphs to eliminate single-derivation redundancy. | Chenghang Shi et al., "Two Birds with One Stone: Multi-Derivation for Fast Context-Free Language Reachability Analysis", ASE 2023, DOI [10.1109/ASE56229.2023.00118](https://doi.org/10.1109/ASE56229.2023.00118). |
| `POCR` / `FOCR` | Paired predecessor/successor reachability trees (POCR) and fully ordered edge-critical-graph closure (FOCR) tame transitive redundancy during on-the-fly solving. | Yuxiang Lei et al., "Taming Transitive Redundancy for Context-Free Language Reachability", OOPSLA 2022, DOI [10.1145/3563343](https://doi.org/10.1145/3563343). |
| `Skewed` | Skewed tabulation: static grammar rewrites plus dynamic propagating-edge tracking reduce wasted and unnecessary summary edges. | Yuxiang Lei et al., "Context-Free Language Reachability via Skewed Tabulation", PLDI 2024, DOI [10.1145/3656451](https://doi.org/10.1145/3656451). |
| `Sqid` | Relation chaining: adaptive and differential chaining over dual old/delta graph views. | Chenghang Shi et al., "Context-Free Language Reachability via Efficient Relation Chaining", OOPSLA 2026, DOI [10.1145/3798270](https://doi.org/10.1145/3798270). |
| `Stg` | Staged solving: decomposes the grammar into a small matching CFG `L` and a regular part `R`, solving each stage separately. | Chenghang Shi et al., "Better Not Together: Staged Solving for Context-Free Language Reachability", ISSTA 2024, DOI [10.1145/3650212.3680346](https://doi.org/10.1145/3650212.3680346). |
| `EndpointQuotient` | Grammar-indexed endpoint-quotient (GEQ) solver: exact least fixed point over endpoint partitions with symbolic nullable diagonals; SCC-classified staged evaluation. | New algorithm in Lotus. |
| `CertCFL` | Exact solving certified by cardinality: saturation tracks per-block node counts, a universal-degree certificate promotes a block to exact once it is dense enough, and any block that cannot be certified is bisected and re-saturated. | New algorithm in Lotus. |

`Engines/Common/` provides shared infrastructure: `BatchSolverEngine` (a
`Relation` adapter for the CAT and IEOCE variants), `Reachability` (the
compressed result shared by the CAT/IEOCE/Skewed engines), `Tabulation`, and
`InputBridge`.

`SolverSession` owns the backend-independent pending-change check. A solve with
no new node or fact returns zero work counters without entering SQID, CERT,
endpoint-quotient, CAT/IEOCE, Skewed, or the classical worklist backends.

## Preprocessing

| Pass | Idea | Paper |
|---|---|---|
| `GraphSimplification` | Port of POCR's SCC elimination and PEG/IVFG graph-folding client passes. | Yuxiang Lei et al., OOPSLA 2022, DOI [10.1145/3563343](https://doi.org/10.1145/3563343). |
| `RSMFoldability` | Recursive-state-machine-guided node-pair foldability analysis (with `RecursiveStateMachine` in `Core`). | Yuxiang Lei et al., "Recursive State Machine Guided Graph Folding for Context-Free Language Reachability", PLDI 2023, DOI [10.1145/3591233](https://doi.org/10.1145/3591233). |

See also `docs/source/cfl/classical/` for detailed per-engine documentation
and the shared `SolverSession` backend list.

## Specialized PEG alias analysis

[`Engines/SubcubicAA`](Engines/SubcubicAA/README.md) implements Qirun Zhang
et al., "Efficient Subcubic Alias Analysis for C", OOPSLA 2014,
DOI [10.1145/2660193.2660213](https://doi.org/10.1145/2660193.2660213).
It computes the paper's memory/value relations with two-phase propagation,
the value-alias seed join, fast-set differences, and component decomposition.
It accepts a validated assignment/dereference PEG through its own API and
`lotus-cfl-subcubic-aa` driver; it is separate from the general grammar
engines listed above.
