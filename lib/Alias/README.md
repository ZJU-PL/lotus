# Alias Analysis

This directory contains the alias- and pointer-analysis implementations used by
Lotus.

## Analysis Comparison Table

| Analysis | Tool/Command | Analysis Type | Flow-Sensitive | Context-Sensitive | Field-Sensitive | Third-Party | Notes |
|----------|--------------|---------------|----------------|-------------------|-----------------|-------------|-------|
| **SparrowAA** | `lotus-alias-sparrow-aa` | Inclusion-based | ❌ No | ✅ Yes (CI, 1-CFA, 2-CFA, etc.) | ❌ No | ❌ No | More graph simplification algorithms; no on-the-fly callgraph construction |
| **AserPTA** | `lotus-alias-aser-aa` | Inclusion-based | ❌ No | ✅ Yes (CI, 1-CFA, 2-CFA, Origin) | ✅ Yes | ✅ Adapted from AserPTA | On-the-fly callgraph construction; supports both field-insensitive and field-sensitive modes |
| **CclyzerAA** | `lotus-alias-cclyzer-aa` | Inclusion-based / Unification | ❌ No | ✅ Yes (CI, `k-CFA`, `k-Caller`) | ✅ Yes | ✅ Vendored (`cclyzer++`) | Optional Soufflé Datalog backend (`LOTUS_ENABLE_CCLYZER`) |
| **SparsePTA** | `lotus-alias-fspta` (`fspta` / `vfspta`) | Inclusion-based | ✅ Yes | ❌ No | ✅ Yes | ❌ No | SVFG/MemorySSA sparse (`FlowSensitivePTA`) and object-versioned (`VersionedFlowSensitivePTA`) solvers |
| **ValueFlowPTA** | `lotus-alias-fspta` (`--analysis=vfpta`) | Inclusion-based | ✅ Yes | ❌ No | ❌ No | ❌ No | Direct LLVM-IR value-flow solver (Li et al., ESEC/FSE'11); no SVFG or pre-analysis |
| **BootstrapAA** | `lotus-alias-bootstrap` | Inclusion-based (bootstrapped) | ✅ Yes | ✅ Yes (exact call path + all-contexts) | ✅ Yes | ❌ No | PLDI'08 partitioning + guarded backward summaries + optional ROBDD path sensitivity |
| **CFL (LLVM)** | - | CFL-reachability | ❌ No | ❌ No | - | ✅ Yes (LLVM) | Via alias wrapper (libLLVMAnalysis) |
| **DDA** | - (library only, `DDAPass`) | Demand-driven value-flow | ✅ Yes | Optional (`FlowDDA` / `ContextDDA`) | ✅ Yes | ❌ No | SVF-style demand-driven value-flow solver implemented natively in Lotus |
| **DyckAA** | `lotus-alias-dyck-aa` | Unification-based | ❌ No | ❌ No | - | ❌ No | - |
| **Dynamic** | `dynaa` (optional) | Dynamic | - | - | - | ❌ No | Runtime analysis (`LOTUS_ENABLE_DYNAA`) |
| **FPA** | `lotus-alias-call-graph` (`-cg-type=fpa-*`) | Type-based | - | - | - | ❌ No | Function pointer analysis |
| **LotusAA** | `lotus-alias-lotus-aa` | Inclusion-based | ✅ Yes | ✅ Yes (function summaries) | ✅ Yes | ❌ No | Native Lotus analysis; flow-, context-, field-, and path-sensitive |
| **seadsa** | `lotus-alias-sea-dsa-dg`, `lotus-alias-seadsa-tool` | Unification-based | ❌ No | ✅ Yes | ✅ Yes | ✅ Yes (SeaDsa) | Context-sensitive heap (heap cloning) |
| **SRAA** | - | Range-based | ❌ No | ❌ No | - | ❌ No (based on CGO'17 paper) | Flow- and context-insensitive |
| **UnderApproxAA** | - | Pattern-based | - | - | - | ❌ No | Must-alias analysis |
| **AllocAA** | - | - | - | - | - | ❌ No | - |
| **TPA** | `lotus-alias-tpa` | Inclusion-based | ✅ Yes | ✅ Yes (CI, `k-CFA`, Adaptive) | Partial | ❌ No | Semi-sparse flow- and context-sensitive analysis with k-limiting |

## Context Sensitivity Variants

| Analysis | Context Variants Supported |
|----------|---------------------------|
| **SparrowAA** | CI (context-insensitive), 1-CFA, 2-CFA |
| **AserPTA** | CI, 1-CFA, 2-CFA, Origin-sensitive |
| **DDA** | Flow-sensitive CI (`FlowDDA`), flow-sensitive CS (`ContextDDA`) |
| **LotusAA** | Context-sensitive (details not specified) |
| **seadsa** | Context-sensitive with heap cloning |
| **TPA** | Context-sensitive with k-limiting |

## Key Differences: SparrowAA vs AserPTA

| Feature | SparrowAA | AserPTA |
|---------|-----------|---------|
| **Callgraph Construction** | No on-the-fly construction | On-the-fly construction (default) |
| **Graph Simplification** | More algorithms integrated | Standard |
| **Field Sensitivity** | Field-insensitive only | Both field-insensitive and field-sensitive modes |
| **Context Variants** | CI, 1-CFA, 2-CFA | CI, 1-CFA, 2-CFA, Origin-sensitive |

## Analysis Characteristics Summary

| Characteristic | Analyses |
|----------------|----------|
| **Inclusion-based** | SparrowAA, AserPTA, CclyzerAA, SparsePTA, ValueFlowPTA, TPA, LotusAA, BootstrapAA |
| **Demand-driven** | DDA |
| **Unification-based** | DyckAA, seadsa |
| **Flow-sensitive** | DDA, SparsePTA, ValueFlowPTA, TPA, LotusAA, BootstrapAA |
| **Context-sensitive** | SparrowAA, AserPTA, CclyzerAA, DDA (ContextDDA), seadsa, TPA, LotusAA, BootstrapAA |
| **Field-sensitive** | AserPTA (optional), CclyzerAA, SparsePTA, DDA, LotusAA, BootstrapAA, seadsa |
| **Specialized** | FPA (function pointers), UnderApproxAA (must-alias), Dynamic (runtime), SRAA (range-based) |

## Subdirectories

`lib/Alias/` is organized by high-level taxonomy:

- `InclusionBased/`: inclusion-style whole-program pointer analysis engines:
  `SparrowAA`, `AserPTA`, `CclyzerAA`, `SparsePTA`, `ValueFlowPTA`, `TPA`,
  `LotusAA`, and `BootstrapAA`. See [`InclusionBased/README.md`](InclusionBased/README.md)
  for the precision-tier selection guide (`FICI` / `FICS` / `FSCI` / `FSCS`).
- `UnificationBased/`: unification/CFL-style analyses such as `DyckAA` and
  `seadsa`.
- `DemandDriven/`: on-demand query-driven analyses such as `DDA`.
- `Specialized/`: targeted analyses such as `AllocAA`, `SRAA`, and
  `UnderApproxAA`.
- `Infrastructure/`: shared support layers such as `AliasAnalysisWrapper`,
  `PtsSet`, `Metrics`, and `Spec`.
- `Dynamic/`: runtime-based alias instrumentation and validation support; it
  sits directly under `lib/Alias/`.

## Comparing precision and soundness: metrics

To compare pointer analyses (e.g. SparrowAA vs TPA vs AserPTA), use the built-in **metrics** layer under `lib/Alias/Infrastructure/Metrics/`:

- **Design**: See [METRICS.md](METRICS.md) for the rationale and mapping from Java points-to metrics (e.g. hybrid context-sensitivity, PLDI’13) to C/C++.
- **API**: `PointerAnalysisMetrics` and `collectMetricsFromWrapper(AliasAnalysisWrapper&, Module&, PointerAnalysisMetrics&)` in `include/Alias/Infrastructure/Metrics/PointerAnalysisMetrics.h`.
- **Metrics**: Points-to set size (avg/median/max), call-graph edges, indirect-call sites and “poly” indirect calls (sites with >1 target).

**Which analyses support which metrics (via the wrapper):**

| Analysis | Points-to size | Indirect-call resolution |
|----------|----------------|---------------------------|
| SparrowAA | ✅ Full | ✅ |
| AserPTA | ❌ Wrapper backend not integrated | ❌ Wrapper backend not integrated |
| TPA | ✅ Size only | ✅ |
| DyckAA, UnderApproxAA, CFLAnders/CFLSteens | ❌ | ❌ |
| Combined | ✅ (via Andersen) | ✅ (via Andersen) |

LotusAA and FPA are separate tools and are not backends in
`AliasAnalysisWrapper`; AserPTA has a wrapper configuration, but the backend
rejects it explicitly instead of falling back to another analysis. SeaDsa,
AllocAA, and the remaining analyses are not integrated in the wrapper. See
[METRICS.md](METRICS.md) for the full support table.

- **High-level clients** (taint, use-after-free, ref-count) need more than alias/points-to (mod/ref, DFA, etc.) and live outside this metrics layer.
