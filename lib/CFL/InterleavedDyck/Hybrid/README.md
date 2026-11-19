# Hybrid Dual-Core Sandwich Interleaved-Dyck Pipeline

This module (`CanaryInterleavedDyckHybrid`, CLI `lotus-cfl-interleaved-dyck hybrid`)
combines condensed shared-RHS Dyck bounds with register-packed `ParitySemiring`
single-sided `AffineSPDS` synchronization into a bounded-memory demand pipeline:

1. **Core 1 — Stage 1 (Corridor Slice + Condensed Boolean SPDS)**:
   Slices the graph in bit-parallel SCC-DAG order to demanded corridors, strips
   unmatched delimiters to a fixpoint, and computes the Boolean projection upper
   bound `U_1 = D_P \cap D_B` via `Neutral`-SCC condensation and shared-RHS Dyck-1
   tabulation (`SPDS/DyckReachability.h`). Short-circuits when `U_1` is empty.
2. **Core 1 — Stage 2a (Demand Union-Dyck Certified Lower Bound)**:
   Evaluates a single Dyck-1 problem over the disjoint union of parentheses and
   brackets (`L_1 = D_{P \cup B}`) on the surviving candidates to certify reachable
   pairs `L_1 \subseteq U_1` in bounded memory (without quadratic all-pairs CNF
   tables). Short-circuits whenever `U_1 == L_1`.
3. **Core 2 — Stage 3 (Single-Sided `ParitySemiring` `AffineSPDS` + Feedback)**:
   Evaluates **only unresolved pairs** `\Delta = U_1 \ L_1` using the best
   asymmetric cross-stack parity observer (`SingleSidedParityOptions` default:
   `groups = 32`, virtual 64-dim, with `corridor_active_rehash = true`
   auto-narrowing to `uint8_t`/`uint16_t` when active delimiter types fit),
   plus dead-edge corridor feedback rounds.

## Guarantees

For every query scope, all non-reflexive pairs satisfy:

```text
certified_lower <= true_interdyck <= final_upper == stage3_affine <= stage1_spds
```

## CLI Usage

```sh
build/bin/lotus-cfl-interleaved-dyck hybrid --queries queries.txt --json graph.dot
build/bin/lotus-cfl-interleaved-dyck hybrid --source 3 --timings --pairs graph.dot
```
