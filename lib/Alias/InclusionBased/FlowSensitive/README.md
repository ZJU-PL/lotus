# Flow-sensitive inclusion-based pointer analysis

The public headers in `include/Alias/InclusionBased/FlowSensitive/` and the
three analysis implementations use matching variant directories:

| Directory | Analysis | Input | Driver mode |
| --- | --- | --- | --- |
| `Sparse/` | `FlowSensitivePTA` | SVFG and MemorySSA | `fspta` |
| `Versioned/` | `VersionedFlowSensitivePTA` | SVFG and object versions | `vfspta` |
| `ValueFlow/` | `ValueFlowPTA` and its `ValueFlowGraph` | LLVM IR | `vfpta` |

Include headers through their variant directory, for example
`Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h`.
The parent `CMakeLists.txt` builds all three variants into the `FlowSensitivePTA`
library used by the alias driver and tests.
The private parallel runtime lives in `Parallel/` and serves the `Sparse/`
public API.

## Parallel execution

```bash
lotus-alias-fspta input.bc --parallel --threads=4 --dump-stats
lotus-alias-fspta input.bc --parallel --threads=4 --verify-parallel
```

`FlowSensitivePTA::Config::parallel` selects a pipeline that evaluates upcoming
transfers on immutable effect snapshots and retires them in the reference
solver's worklist order. Worker blocks forward predicted effects privately;
version and producer checks reject stale predictions. Certified unchanged
effects can be reused without re-running a transfer. Indirect-call connectors
run on the calling thread after evaluators drain.

Use `--parallel-block-size=1`, `--parallel-memo=false`, and
`--parallel-share-sets=false` for ablation experiments. Worker snapshots use
shared immutable sets; selecting hash-consed public storage interns the final
results. The unordered experimental policy (`--parallel-order=unordered`)
can choose a different fixed point on order-sensitive transfers.
Precise memory reads check object contents and entry presence; missing entries
also validate the wildcard namespace. `--parallel-object-certificates=false`
selects whole-channel validation for comparison.

Both solvers accept `Config::timeLimitSeconds` (CLI: `--time-limit`). On
expiry the solve stops at the current worklist boundary, keeps the partial
solution queryable, and reports `timedOut()`. `--stats-report` prints the timer
breakdown (solve/labeling/OTF) and `svfgHasSU`.
The versioned solver labels footprint representatives concurrently
(`Config::versioningThreads`, CLI: `--versioning-threads`; 0 selects
`hardware_concurrency()`, 1 runs sequentially) and merges the results in object
order, so the labels are independent of thread scheduling.
`FlowSensitivePTA` also has `writeAnalysisResultToFile` /
`readAnalysisResultFromFile` (`LOTUS_SPARSEPTA 1`). The versioned solver
writes `LOTUS_VFSPTA 2` and reads versions 1 and 2. The driver exposes both
through `--write-pts` / `--read-pts`. These formats store Lotus object IDs.

## Configuration and SVF parity notes

### On-the-fly indirect-call bound

On-the-fly (OTF) call-graph construction is bounded by the pre-analysis. Both
solvers accept `Config::isIndirectCallTargetAllowed`, a predicate evaluated
before an OTF edge is materialized; it is the equivalent of SVF's use of the
auxiliary Andersen call graph in `FlowSensitive::updateCallGraph`. Leaving it
empty removes the bound, which is only safe for graphs without indirect calls.
The parallel pipelines only propose candidates in workers; the calling thread
applies the bound when it retires them, so each candidate is judged once. Two
counters report the outcome:

- `Statistics::indirectCallsRejected` — candidate edges the bound rejected.
- `Statistics::unresolvedIndirectCalls` — indirect callsites reached while no
  `connectIndirectCall` connector was configured (silent in SVF).

`lotus-alias-fspta` installs the pre-analysis target list for both roles.

### Dynamic field-insensitive collapse

`SVFG::getOrCreateFieldInsensitiveObject()` and
`SVFG::getFieldInsensitiveObject()` mirror SVF's
`SVFIR::setObjFieldInsensitive` / `getFIObjVar`. When a GEP reaches the solver
without `GepAccessInfo` (a graph built, loaded, or hand-constructed without
that annotation, or SVF's variant field GEP), the GEP transfer collapses the
struct into a dedicated field-insensitive representative instead of keeping
per-field objects. `Statistics::fieldInsensitiveCollapses` counts objects
collapsed during solving; a collapse the SVFG builder already performed is not
counted again.

Object IDs are allocated through `SVFG::allocateObjectId()` so that
solve-time object creation and builder allocations share one counter and
cannot collide.

### Versioned object ordering

`VersionedFlowSensitivePTA::Config::orderVersionedObjectsByWeight` (formerly
`clusterVersionedObjects`) only controls the order in which objects are
labelled: footprint representatives with more occurrences are processed first.
It does **not** merge objects, equivalents, or SVFG node IDs; SVF's
`cluster()` performs those merges and has no counterpart here.

### Statistics

`FlowSensitivePTA::statisticsReport()` additionally reports
`solveIterations`, per-node-kind transfer counts
(`processedAddr`, `processedCopy`, `processedGep`, `processedLoad`,
`processedStore`, `processedPhi`, `processedActualParam`,
`processedFormalRet`, `processedMSSA`), top-level points-to sizes
(`maxTopLevelPtsSize`, `averageTopLevelPtsSize`), the indirect-call bound
counters, and `fieldInsensitiveCollapses`.
`VersionedFlowSensitivePTA::statisticsReport()` adds version points-to sizes
(`nonEmptyVersions`, `maxVersionPtsSize`, `averageVersionPtsSize`) and the same
bound/collapse counters.

## Standalone engine status

`FlowSensitivePTA` and `VersionedFlowSensitivePTA` are standalone engines in
this tree: they are driven by `lotus-alias-fspta`, the tests, and direct API
use. They are not registered as an `AliasAnalysis` implementation, so
framework clients that require that interface cannot select them by name.
Wrapping them for the framework would need a call-graph/pre-analysis setup
step and result caching, which this module does not provide.

`FlowSensitivePTA` is the thread-independent sparse solver. It maintains:

- top-level points-to sets for pointer-producing SVFG nodes;
- per-node, per-object MemorySSA `IN` and `OUT` points-to state;
- explicit Addr/Copy/GEP/Phi/Load/Store and parameter-flow transfer;
- canonical `(allocation, normalized byte offset)` field objects, with array
  indices collapsed for field-insensitive updates;
- field-offset-aware aggregate global initializers and `memcpy`/`memmove`;
- singleton-object strong updates and conservative weak updates;
- Tarjan SCC decomposition with SCC-local fixed points and successor requeueing;
- auxiliary-PTA call-graph initialization plus on-the-fly indirect-call
  connection followed by SCC reconstruction;
- selectable mutable and hash-consed points-to set storage.

The concurrency layer does not duplicate this solver. `FSMPTA` runs it over an
SVFG augmented with fork/join and `ThreadMHPIndirectVF` edges. The multi-stage
slicer (MSli) supplies an
optional filtered solve graph.

This module contains three flow-sensitive analyses:

- `FlowSensitivePTA` implements the default exhaustive `fspta` analysis.
- `VersionedFlowSensitivePTA` implements the `vfspta` analysis, which versions
  each abstract object and keys memory facts by that version instead of by
  SVFG location. Its mechanisms are:
  - object prelabeling and meld versions
  - consume/yield maps
  - version and statement reliance
  - strong and weak updates
  - intrinsic memory definitions
  - footprint-equivalent object reuse
  - occurrence-weighted propagation
  - on-the-fly (OTF) delta-edge updates
  - result persistence (`LOTUS_VFSPTA`, version 2 with version 1 reads)
  - relabeling on a topology change only when recursion changes the
    strong-update status of an existing site
- `ValueFlowPTA` implements the value-flow formulation of Li, Cifuentes, and
  Keynes (ESEC/FSE 2011). It builds a field-insensitive value-flow graph
  directly from LLVM IR, orders indirect-flow construction by object escape,
  and applies Algorithm 2's store-plus-IDF sparse reaching-definition solve to
  its interprocedural supergraph. It does not require a precomputed SVFG or
  flow-insensitive pre-analysis.

Select the versioned solver with `lotus-alias-fspta --analysis=vfspta`.
Select the direct value-flow solver with
`lotus-alias-fspta --analysis=vfpta`.
