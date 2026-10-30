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
`readAnalysisResultFromFile` (`LOTUS_SPARSEPTA 2`; version 1 is still read).
The versioned solver writes `LOTUS_VFSPTA 2` and reads versions 1 and 2. The
driver exposes both through `--write-pts` / `--read-pts`. These formats store
Lotus object IDs; version 2 also records the field-insensitive identity map
(`M` records for `fspta`, `F` records for `vfspta`) so callers can report a
representative as its base object.

### Clustering, pointer-only SVFG, and prelabel seeds

The pointer-only and prelabel options follow SVF. Object merging is a separate
Lotus abstraction policy:

| Driver flag | Config | Effect |
| --- | --- | --- |
| `--cluster-objects=none` | `ClusterMethod::None` | Identity mapping (SVF `plainMap()`), the default |
| `--cluster-objects=type` | `ClusterMethod::Type` | Merge base objects with the same LLVM type and stack/heap/global/singleton kind (both solvers) |
| `--cluster-objects=footprint` | `ClusterMethod::Footprint` | Merge objects with identical access footprints (`vfspta` only) |
| `--pointer-only[=false]` | `pointerOnlySVFG` | Model only pointer-typed loads/stores, like SVF's `memSSA.buildPTROnlySVFG`; default enabled |
| `--vfspta-prelabel=builder` | `preAnalysisTargets` empty | Seed meld labels from the builder's MemorySSA region sets (ablation) |
| `--vfspta-prelabel=pre-analysis` | `preAnalysisTargets` set | Seed meld labels from the auxiliary points-to result (default, following SVF's `prelabel()`) |

Clustering is a sound coarsening for a may-points-to analysis: the object map
replaces each object with its class representative and every memory/version
fact is keyed by that representative. Strong updates are disabled on any class
with more than one member. SVF's `cluster()` instead permutes IDs for bitvector
locality and preserves object identity and strong updates. The standalone
`BitvectorClusterer` helper supplies a sparse permutation, but is not wired into
the solvers' set storage.
`Statistics::clusteredObjects` / `mergedObjectClusters` report the size of the
partition. A written result file stores post-clustering object IDs, so reload
it with the same `clusterObjects` setting that produced it.

The `pre-analysis` seed requires the SVFG builder's auxiliary PTA
(`SVFGBuilder::getPreAnalysisPointsTo`). Without it the driver keeps the
builder-recorded region sets, which is what hand-built or pre-analysis-free
graphs must use.

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
- `Statistics::unboundedIndirectCalls` — OTF edges materialized while no
  `isIndirectCallTargetAllowed` predicate was configured. A non-zero value
  means the call graph is unbounded, unlike SVF's auxiliary-Andersen bound;
  both reports print a `WARNING` line in that case.

`lotus-alias-fspta` installs the pre-analysis target list for both roles. It
connects a callee only after the flow-sensitive function-pointer result reaches
it; it does not eagerly connect every auxiliary-analysis candidate.

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

SVF has no separate field-insensitive object (`getFIObjVar` returns the base),
while Lotus keeps a dedicated representative whose `baseObjId` is the base.
Solving must use the representative, but reporting should not: result-file
version 2 records that identity (`M` in `LOTUS_SPARSEPTA`, `F` in
`LOTUS_VFSPTA`) and both solvers expose `canonicalObject(object)`, which maps a
representative back to its base and returns every other object unchanged.

### Versioned object ordering and SCC scheduling

`VersionedFlowSensitivePTA::Config::orderVersionedObjectsByWeight`
(formerly `clusterVersionedObjects`) only controls the order in which objects
are labelled: footprint representatives with more occurrences are processed
first. Object merging is selected separately through `Config::clusterObjects`
(see above).

Both solvers order their statement worklist by the SVFG's strongly connected
components (shared helper in `Support/SVFGOrder.h`): components are emitted
in reverse topological order and only the
consumers of a changed statement are revisited. The versioned solver keeps a
priority queue keyed by `(SCC index, NodeID)`, so the order is deterministic.
`Statistics::sccs` / `maxSccSize` / `solveIterations` report the decomposition
and the number of completed sweeps.

### Address and external-call modeling

Constant-expression GEP operands and GEP instructions share normalized member
offsets: array indices collapse while struct members remain distinct. An empty
flow-sensitive base set stays empty; it does not import auxiliary targets.
Alias queries expand field-insensitive objects and treat unknown pointees as
may-alias, following SVF.

The shared SVFG builder uses `AliasSpecManager` return rules for direct and
resolved indirect external calls. Argument aliases and fresh allocations are
represented by ordinary copy/address edges. Unmodeled pointer returns and
unsupported static/dereferenced return alternatives use an unknown object.
This does not supply complete external memory-write summaries; external
side-effect modeling remains a separate limitation.

Top-level propagation accumulates facts. A pointer store waits for a non-empty
LHS before forwarding memory, so an early load cannot retain a fact from before
a later strong update. Version propagation seeded by global initializers stays
queued when the statement solver starts.

### Statistics

`FlowSensitivePTA::statisticsReport()` additionally reports
`solveIterations`, per-node-kind transfer counts
(`processedAddr`, `processedCopy`, `processedGep`, `processedLoad`,
`processedStore`, `processedPhi`, `processedActualParam`,
`processedFormalRet`, `processedMSSA`), top-level points-to sizes
(`maxTopLevelPtsSize`, `averageTopLevelPtsSize`), the SVF-named population and
IN/OUT sizes (`totalPointers`, `totalObjects`, `fieldInsensitiveObjects`,
`fieldSensitiveObjects`, `maxPtsSize`, `averagePtsSize`, `addrTakenVars`,
`maxAddrTakenPtsSize`, `averageAddrTakenPtsSize`), the indirect-call bound
counters, `fieldInsensitiveCollapses`, and the clustering counters.
`VersionedFlowSensitivePTA::statisticsReport()` adds version points-to sizes
(`nonEmptyVersions`, `maxVersionPtsSize`, `averageVersionPtsSize`), version
counts (`totalVersions`, `maxVersionsForObject`, `singleVersionObjects`,
`totalExistingVPts`, `totalEmptyVPts`), the same population/address-taken and
clustering counters, and the SCC/sweep counters.

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
- pointer-only load/store modelling (`Config::pointerOnlySVFG`, matching SVF's
  pointer-only SVFG) with an opt-out for the previous behaviour;
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
  - object prelabeling and meld versions (auxiliary points-to seeds by default;
    region seeds with `--vfspta-prelabel=builder`)
  - consume/yield maps
  - version and statement reliance
  - strong and weak updates, disabled on multi-object clusters
  - intrinsic memory definitions
  - footprint-equivalent object reuse and optional object clustering
  - occurrence-weighted propagation
  - SCC-ordered statement scheduling
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
