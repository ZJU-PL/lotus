# BootstrapAA

An independent inclusion-based analysis for Lotus, with an LLVM 14 adapter,
flow-sensitive memory states, and context-sensitive interprocedural summaries.
The public C++ namespace is `lotus::bootstrap`.

## Relationship to the bootstrapping paper

The design is based on Vineet Kahlon, **Bootstrapping: A Technique for Scalable
Flow and Context-Sensitive Pointer Alias Analysis**, PLDI 2008, pp. 249-259.

This implementation follows the paper's **bootstrapping framework**. It is **not
a reproduction of its compact, guarded update-sequence summary algorithm**.
The distinction is important:

| Paper mechanism | Implementation |
| --- | --- |
| Steensgaard partitioning, §2.1 | Unary unification graph with recursive shape merging. Its cycle-collapsed points-to hierarchy, depths, and value/object component maps are retained. Top connects all groups. |
| Relevant variables/statements, Algorithm 1 | Hierarchy-aware backward fixed-point closure over scalar definitions, loads, higher/cyclic store destinations, PHI edges, formals/actuals, returns, and indirect-call targets. CFG and call structure remain intact. |
| Thresholded Andersen refinement, §2.1 | Inclusion constraints are solved on the coarse partition's dependency slice only when its size exceeds `andersen_threshold` (default 60). |
| Overlapping Andersen cover, Theorem 7 | Inverse points-to sets, not connected components of the undirected alias graph. Top-valued pointers occur in every candidate inverse set. Empty sets receive singleton coverage. Duplicate clusters are removed, but overlap is preserved. |
| Accurate per-cluster analysis | On-demand CFG fixed points with strong/weak memory updates and input-state-tabulated function summaries. Results are unioned over all clusters containing the queried pointer. |
| Summary tuples, Definition 8 / Algorithms 4–5 | **Different representation:** a summary maps a projected entry state to possible exit memory and return values. No implementation of the paper's guarded backward tuple representation is claimed. |
| FSCI/hierarchy dovetailing, Algorithm 2 | **Not implemented.** The forward solver resolves stores using the current flow/context-sensitive state instead. |
| Recursive summary convergence | Dependency-driven fixed point across function/input-state records, prioritized by an explicit conservative call-graph SCC condensation; no call-string depth cutoff. |
| Parallel per-cluster execution | Missing clusters selected by a query are built by a bounded standard-thread worker set. Solver state and statistics are isolated and published deterministically. |

The summary representation is a deliberate engineering adaptation, not a claim
that it has the succinctness or performance reported for the paper's summaries.
Points-to sets use a dependency-free sparse word-bitset; maps remain for summary
keys and caches. No scalability benchmark or proof of LLVM-level soundness
accompanies this implementation.

### Implementation choices and engineering details

1. The unification phase retains a cycle-collapsed Steensgaard hierarchy DAG,
   component membership, predecessor/successor edges, depths, and cyclic flags.
2. Relevance slicing uses strict hierarchy ancestors and cyclic components when
   identifying indirect stores and havocs. The unsliced solver remains a
   differential oracle; this is still a normalized-IR adaptation of Algorithm 1.
3. Points-to sets use sparse 64-bit words with linear union/intersection and
   ascending iteration instead of per-element tree nodes.
4. Statistics expose partition and cluster distributions, hierarchy shape,
   cover overlap, lazy cluster evaluation, context-cache behavior, summary
   updates, preprocessing time, and per-cluster solve time.
5. The LLVM adapter recognizes additional allocation functions, models
   ``realloc``'s old/fresh/null alternatives, preserves zero-length intrinsics,
   resolves direct calls through aliases, and summarizes common read-only
   interior-pointer functions.
6. Large partitions are processed largest-first. The Andersen threshold is
   lowered after effective refinements. A refinement that reduces the largest
   cluster by less than 25% is rejected, and the threshold is raised to skip
   smaller partitions. Adaptive partition-size and partition-by-hierarchy work
   guards avoid starting unbounded refinement work; all controls remain
   selectable.
7. Function summaries are scheduled by reverse call-graph-SCC priority, and
   independent overlapping clusters are evaluated in parallel with deterministic
   aggregation.

### Remaining differences from the paper

The largest gaps are:

1. Algorithms 2 and 3 are absent. FSCI facts are obtained from the active
   flow/context-sensitive state instead of being computed by hierarchy-ordered
   dovetailing.
2. Definition 8 and Algorithms 4 and 5 are replaced by input-state summaries.
   These summaries are semantically useful, but can enumerate many projected
   memory states and do not carry guarded positive/negative points-to and alias
   constraints.
3. Summary size, speedup, and peak memory are not yet measured on the paper's
   benchmarks; the structural and timing counters needed for that measurement
   are exposed.

A paper-faithful implementation should proceed in dependency order:

1. Implement hierarchy-ordered FSCI computation from Algorithms 2 and 3.
2. Add an interned guard language for points-to, non-points-to, alias, and
   non-alias atoms; then implement backward tuple transfer and call-graph-SCC
   summary convergence from Algorithms 4 and 5.
3. Keep the current input-state solver as a reference backend until guarded
   summaries agree on bounded programs, recursion, and indirect calls.
4. Evaluate the adaptive and parallel implementations on the PLDI benchmarks.

Separate LLVM-model limitations affect soundness outside the documented input
contract rather than fidelity to the paper: concurrent interference, signals,
external reentrant callbacks, dynamic loading, detailed byte layouts, and
unsupported pointer-representation operations require preprocessing or
additional conservative models.

## Source layout and targets

- `include/Alias/InclusionBased/BootstrapAA/Engine.h`: LLVM-independent IR,
  lattice, query/status/options API.
- `include/Alias/InclusionBased/BootstrapAA/BootstrapAA.h`: LLVM-facing API.
- `lib/Alias/InclusionBased/BootstrapAA/Engine.cpp`: preprocessing, clustering,
  slicing, tabulation, and program-point/context queries.
- `lib/Alias/InclusionBased/BootstrapAA/BootstrapAA.cpp`: LLVM lowering.
- `tools/alias/lotus-alias-bootstrap.cpp`: command-line driver.
- `tests/unit/Alias/InclusionBased/BootstrapAA/`: GTest engine and LLVM adapter
  regressions.

`CanaryBootstrapCore` is the dependency-free C++17 engine. `CanaryBootstrapAA`
adds LLVM Core/Support. `lotus-alias-bootstrap` additionally links LLVM IRReader.
The analysis is registered in the existing Alias library, unit-test, tool, and
documentation trees. It is not an `AliasAnalysisWrapper` backend because its
queries require a program point and optionally a call context.

## Build in LOTUS

From a Lotus checkout:

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON \
  -DLLVM_BUILD_PATH=/path/to/llvm-14/lib/cmake/llvm
cmake --build build --target CanaryBootstrapAA lotus-alias-bootstrap \
  alias_tests
ctest --test-dir build -R '^alias_tests\.Bootstrap' --output-on-failure
```

## Program-point and context queries

```cpp
#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace lotus::bootstrap;

// M must remain alive and unchanged. The default entry is a defined main.
BootstrapAA analysis(M);

// value is an LLVM scalar pointer value in M; location is an instruction in M.
// Before means immediately before location; After includes location's effect.
QueryResult result = analysis.pointsTo(value, location, {}, Point::Before);

// Query inside a callee reached by this exact sequence of call instructions,
// ordered from the entry activation to the queried activation.
BootstrapAA::CallContext path{&outer_call, &inner_call};
QueryResult contextual = analysis.pointsTo(callee_value, callee_location, path);

// This API explicitly forgets the call context by unioning reachable activations.
QueryResult joined = analysis.pointsToAllContexts(callee_value, callee_location);

if (result.status == QueryStatus::ResourceLimit) {
  // result.points_to is top, never an incomplete/under-approximated set.
}
for (Id object : result.points_to.objects()) {
  const llvm::Value *site = analysis.allocationSite(object);
  // UNKNOWN and NULL_OBJECT have no LLVM allocation site (site == nullptr).
  // objectInfo(object) supplies the kind, name, and singleton classification.
  (void)site;
}

const SteensgaardHierarchy &hierarchy = analysis.hierarchy();
// Components form a cycle-collapsed DAG; edges point toward one dereference.
(void)hierarchy.depth;

// Optional: eagerly build every cluster solver using configured parallelism.
analysis.precomputeAll();
```

An empty call context means **the entry activation**, not “any caller.” A
context contains call-site identities, not merely function names, so two calls
to the same function can be distinguished. Recursion is represented by repeating
the appropriate call site. Invalid IDs or pointers from another module produce
an exception. A well-formed context that cannot reach the site produces
`Unreachable`.

At a PHI block entry, all PHI results have already been assigned simultaneously
on the incoming edge. A query at a PHI therefore observes the edge-merged PHI
bindings, including with `Before`.

| Status | Meaning |
| --- | --- |
| `Complete` | The modeled analysis reached its fixed point. This does **not** mean the points-to set is exact; it can contain top or other conservative imprecision. |
| `Unreachable` | No modeled activation at the requested context/point. The returned set is empty. |
| `ResourceLimit` | Context or solver-step budget exhausted. The returned set is top. |

`PointsToSet::top()` is canonically `{UNKNOWN}` and denotes every possible
address, not one distinguished heap object. `mayAlias` never uses an unreachable
query to prove disjointness and treats top conservatively. This API does not
produce must-alias facts or implement an LLVM `AAResultBase` pass: that interface
would discard the required program point and call context.

## Execution and memory model

The model is sequential and closed-world, starting at the selected entry after
static initialization. Root pointer parameters and unknown external objects are
top. Ordinary scalar pointer global initializers are modeled; the presence of
`llvm.global_ctors` causes global initial contents to be conservatively unknown
rather than pretending constructors had no effects. Constructors are not exposed
as pre-entry query contexts. A custom entry defines a fresh analysis root; it is
not an arbitrary API call after an unmodeled history of other entry calls.

The LLVM adapter models pointer addresses, bitcasts, in-bounds or zero-offset
GEPs, loads/stores, selects, parallel PHIs, direct/indirect calls, pointer
arguments/returns, and recursive calls. Nonzero, non-in-bounds pointer arithmetic,
integer-to-pointer conversions, address-space casts, freeze, pointer extraction
from aggregates/vectors, and other unsupported pointer producers produce top.

Storage abstraction is allocation-site-based and **field-insensitive**. This
intentionally differs from the paper's flattened-field treatment. Scalar pointer
globals and nonrecursive, entry-block, single-cell static allocas can receive
strong updates when the destination set is exactly one object and the write
covers its pointer-sized cell. Heap sites, aggregate objects, loop allocations,
and recursive-frame allocations are never treated as unique concrete cells.
Repeated allocations therefore cannot cause an unsound strong kill. Non-default
address-space null constants and modules admitting `null_pointer_is_valid` are
conservatively lowered to unknown rather than assuming address zero is unusable.

Byte-width mismatches and non-pointer writes conservatively invalidate pointer
contents. Aggregate/heap byte layouts are not reconstructed: pointer reads/writes
through such layouts generally yield unknown contents. The normalized engine can
also operate on abstract indivisible pointer cells (`read_bytes == write_bytes ==
0`); that is a different, explicit input model, not the LLVM byte model.

Direct, declaration-only `malloc`, `calloc`, `aligned_alloc`, `valloc`,
`memalign`, and common scalar/array `operator new` calls with expected
argument shapes create non-singleton heap sites. Fallible allocators include
null; throwing `operator new` does not. `realloc` includes the original
object, a fresh site, and null. Heap contents start unknown. `free` does not
set the caller's pointer to null or remove the abstract site. Custom allocator
modeling is not supplied.

`byval` parameters get distinct implicit summary storage with unknown contents;
they are not bound as writable aliases of the caller's original aggregate.
Opaque external calls return top and havoc tracked memory unless read-only
attributes or recognized read-only library summaries forbid writes. Common
interior-pointer searches return their input object or null. Unresolved indirect
calls retain an opaque alternative and all possible module targets. Zero-length
memory intrinsics have no effect; other memory intrinsics and atomic memory
operations conservatively havoc their destinations. Invoke/callbr include an
opaque effect alternative, so normal-exit memory is not incorrectly reused as
the only exceptional outcome. Inline assembly is opaque.

This does not model concurrent interference, signal handlers, arbitrary external
callbacks/reentrant execution, dynamic loading, or detailed lifetime/provenance
semantics. Branch predicates are not solved: the analysis is flow-sensitive but
path-insensitive. Clients needing those properties need additional modeling.

## Summary convergence and limits

A summary key is `(function, relevant actual points-to sets, projected memory)`.
A summary contains reachable exit memory and returned points-to sets. Callee
registers do not overwrite caller registers. A missing callee summary is bottom
and suspends that return path; it is never an identity summary. When a summary
changes, dependent callers are rescheduled. The finite allocation-site universe
makes the context/state domain finite, but potentially very large.

The default limits are 4,096 input-state contexts and 1,000,000 refinement steps
**per cluster solver**. Reaching either marks every query using that failed
solver as `ResourceLimit` with top. These limits do not bound preprocessing.
No call-depth truncation, silent worklist cutoff, or partial result is used to
claim disjointness. Cached solvers are reused across queries for the same
cluster; this cache is not thread-safe.

## Driver

```sh
./build/bin/lotus-alias-bootstrap input.bc
./build/bin/lotus-alias-bootstrap --all-contexts \
  --andersen-threshold=1 --threads=8 --max-contexts=4096 \
  --max-steps=1000000 input.bc
```

The default prints pointer-producing instructions in the entry activation,
after each instruction. `--all-contexts` deliberately unions contexts and prints
all functions. The API above is the way to request a specific nonempty context.
`--detailed-stats` prints partition and cluster size distributions plus
per-cluster solve times; aggregate paper-oriented counters are always printed.
`--adaptive-threshold=false` selects the fixed threshold policy, while
`--parallel-clusters=false` selects deterministic serial cluster construction.
`--max-andersen-partition=0` disables the adaptive refinement-size guard.
`--max-andersen-work=0` disables the estimated refinement-work guard.
`--precompute-clusters` eagerly evaluates disjoint as well as overlapping
clusters; `--all-contexts` enables this automatically.
Exit codes are 0 for completed queries, 1 for input/configuration errors, and 2
when any printed query required a resource-limit fallback. Statistics go to
standard error.

## Validation

The LLVM-independent engine and LLVM 14 adapter build in Lotus. The
GTest suite contains 27 engine groups and eleven LLVM-facing groups; all pass. One
engine group compares sliced/clustered and monolithic configurations on 200
deterministically generated programs. The CLI smoke test also passes on the
included regression IR.

The engine suite additionally passes with C++17, ``-Wall -Wextra -Wpedantic
-Werror``, AddressSanitizer, and UndefinedBehaviorSanitizer. These checks are
joined by ThreadSanitizer coverage of overlapping-cluster, eager-precompute,
and differential parallel execution. These checks are regression and
metamorphic evidence, not a soundness proof or a reproduction of the paper's
performance results.
