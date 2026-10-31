# Pointer Analysis Metrics and Clients (lib/Alias)

This document describes how to compare **precision** and **soundness** of the pointer analyses in lotus using built-in metrics and low-level "clients" over the alias/points-to results. The design is inspired by metrics used in Java points-to literature (e.g. hybrid context-sensitivity, PLDI’13) but adapted for **C/C++** and for analyses that may not expose full points-to sets.

## Why metrics?

- **Precision**: smaller points-to sets, fewer call targets, more NoAlias/MustAlias answers ⇒ better.
- **Soundness**: all analyses in lotus are intended sound (over-approximate); metrics help compare *how much* over-approximation each one incurs.
- **Reproducibility**: same program + same analysis config ⇒ comparable numbers across runs and across analyses.

## Java vs C/C++

Typical Java points-to metrics (e.g. from hybrid-context PLDI’13) include:

| Java metric | Meaning | C/C++ analogue in lotus |
|-------------|---------|--------------------------|
| **Average points-to set size** | Avg # heap objects per variable | Same: avg # abstract targets per pointer (when backend exposes pts) |
| **Call-graph edges** | # edges in the computed call graph | Same: # (call site, callee) edges from indirect resolution |
| **Poly virtual calls** | Virtual calls that could not be de-virtualized | **Indirect call sites with >1 target** (or total targets per site) |
| **May-fail casts** | Casts that could not be proven safe | **Optional**: unsafe pointer casts (e.g. `int2ptr`), or **may-null** pointers at dereferences |

Important differences:

- C/C++ has no “virtual call” or “cast” in the same sense; we use **indirect call resolution** and optionally **cast/null**-related queries.
- Some backends (e.g. DyckAA, SeaDsa) expose **alias** but not a classical **points-to set**; for them we can still use **alias-based** or **query-based** metrics (e.g. sample of alias pairs, or indirect-call resolution via alias with known targets).
- **High-level clients** (taint, use-after-free, ref-count) need more than alias/points-to (e.g. mod/ref, call graph, DFA); they are out of scope for this “metrics over pointer analysis only” layer.

## Proposed metrics (C/C++)

### 1. Points-to set size statistics (when available)

- **Num pointer variables**: number of tracked pointer values (e.g. in the module or at interesting sites).
- **Average / median / max points-to set size**: over those pointers for which the backend provides a points-to set (or a size).
- Backends that do not expose points-to (e.g. alias-only) can leave these as “N/A” or zero; the wrapper can expose `getPointsToSetSize()` where possible.

### 2. Call-graph / indirect-call metrics

- **Num indirect call sites**: number of call sites with indirect callee.
- **Num call-graph edges**: total (call site, callee) edges from the analysis (direct + resolved indirect).
- **Num “poly” indirect calls**: indirect call sites with more than one possible callee (analogue of “could not de-virtualize”).
- **Average targets per indirect call**: total resolved targets / num indirect call sites.

These require the analysis (or a thin client on top of the wrapper) to resolve the called value to a set of `Function*`. The wrapper can support this either by:

- Using **getPointsToSet(calledValue)** and taking elements that are `Function*` (e.g. SparrowAA), or
- Backend-specific hooks that return call targets (e.g. TPA, LotusAA, FPA already have internal call resolution).

### 3. Alias query statistics (optional, can be slow)

- Count **NoAlias / MustAlias / MayAlias / PartialAlias** over a set of pointer pairs.
- More **NoAlias** (and MustAlias) and fewer MayAlias ⇒ more precise; **fraction NoAlias** is a useful precision proxy.
- **Cost**: Enumerating all pointer pairs is O(n²) and too slow. We use **use-site pairs** only: at each instruction, take pointer operands (including the instruction result if it’s a pointer) and query each unordered pair. Pairs are **deduplicated** globally (each (v1,v2) queried at most once). A **cap** (`CollectMetricsOptions::max_alias_pairs`, default 50k) limits how many pairs are queried so metrics stay affordable. Set to **0** to skip alias-pair metrics entirely.
- Works with any backend that implements `query(v1,v2)`.

### 4. Optional C-specific “clients”

- **May-null at dereference**: count pointer dereferences for which the analysis says the pointer may be null (if the backend supports `mayNull()`).
- **Unsafe casts**: count uses of `int2ptr` / `ptr2int` or reinterpret casts that could be proven redundant (requires a clear definition of “redundant” and possibly more than alias).

These can be added later without changing the core metrics layout.

## Implementation approach

- **Unified struct**: `PointerAnalysisMetrics` includes pts size, call-graph/indirect-call, and alias-pair counts (num_alias_pairs_queried, num_no_alias, num_must_alias, num_may_alias, num_partial_alias; `fractionNoAlias()`).
- **Collection**:
  - **Wrapper-based**: iterate module (pointers, indirect call sites), call `getPointsToSet` / `getPointsToSetSize` / `getIndirectCallTargets`; optionally collect alias-pair metrics over use-site pairs with a cap (see `CollectMetricsOptions`).
  - **Backend-specific**: allow each analysis to optionally fill or extend the same struct from its internal state.
- **Alias-pair metrics**: Use-site pairs only, deduplicated, capped by `options.max_alias_pairs` (0 = skip). Default 50k keeps cost reasonable; increase for a larger sample.

## High-level clients (out of scope here)

Clients like **taint analysis**, **use-after-free**, or **ref-count checking** need:

- Alias/points-to (what we provide),
- Plus: mod/ref, call graph, control flow, and often custom DFA.

So they sit **above** the pointer analysis layer and are not part of this metrics module. The metrics defined here are “pointer-analysis-only” so that comparisons are fair and implementation is simple.

## Metrics support by analysis (current wrapper)

Only analyses that are **initialized by the wrapper** and expose points-to or call resolution contribute to the metrics. Summary:

| Analysis | In wrapper? | Points-to size | Indirect-call resolution | Notes |
|----------|-------------|----------------|---------------------------|-------|
| **SparrowAA** | Yes | ✅ Full (`getPointsToSet` + size) | ✅ Via `getPointsToSet` (filter to `Function*`) | Full metrics. |
| **AserPTA** | No (config rejected) | ❌ | ❌ | Config parsing accepts `AserPTA_*`, but the wrapper fails initialization explicitly. |
| **TPA** | Yes | ✅ Size only (`getPointsToSetSize`) | ✅ Via `getIndirectCallTargets` | Uses TPA’s `getCallees(inst)` in wrapper. |
| **DyckAA** | Yes | ❌ | ❌ | Alias only (`getAliasSet`); no pts in wrapper. |
| **UnderApprox** | Yes | ❌ | ❌ | Must-alias only; no pts. |
| **CFLAnders / CFLSteens** | Yes | ❌ | ❌ | Alias queries only; no pts in wrapper. |
| **Combined** | Yes | ✅ Via Andersen | ✅ Via Andersen | Uses SparrowAA + DyckAA; pts from Andersen. |
| **SeaDsa, AllocAA, BasicAA, …** | No | — | — | The wrapper does not initialize them. |
| **LotusAA, FPA** | No | — | — | Separate tools (`lotus-alias-lotus-aa`, `lotus-alias-call-graph`); not backends in AliasAnalysisWrapper. |

So **points-to size** metrics are filled for **SparrowAA, TPA, Combined**, and **indirect-call** metrics are filled for **SparrowAA, TPA, Combined** via the wrapper’s `getIndirectCallTargets()` (SparrowAA uses points-to; TPA uses its internal `getCallees()`).

## Files

- **Design**: this file (`lib/Alias/METRICS.md`).
- **API**: `PointerAnalysisMetrics` and `collectMetricsFromWrapper()` in `lib/Alias/Infrastructure/Metrics/` (see header and implementation).
- **Wrapper**: `getPointsToSetSize()` for SparrowAA and TPA; `getPointsToSet()` for SparrowAA; `getIndirectCallTargets()` for SparrowAA and TPA.

## References

- Hybrid context-sensitivity (Kastrinis & Smaragdakis, PLDI’13): precision metrics include average points-to set size, call-graph edges, poly virtual calls, may-fail casts.
- DOOP/Java: same metrics on Datalog-based points-to analyses.
