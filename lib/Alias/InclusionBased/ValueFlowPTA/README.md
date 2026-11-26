# ValueFlowPTA: Direct Value-Flow Pointer Analysis

`ValueFlowPTA` implements the flow-sensitive, context-insensitive, field-insensitive
value-flow formulation of Lian Li, Cristina Cifuentes, and Nathan Keynes
(*Boosting the Performance of Flow-sensitive Points-to Analysis using Value Flow*,
ESEC/FSE 2011).

Unlike [`SparsePTA`](../SparsePTA/README.md), `ValueFlowPTA` builds its own
value-flow graph (`ValueFlowGraph`) and per-function control graphs directly from
LLVM IR without requiring a precomputed SVFG, MemorySSA, or a flow-insensitive
pre-analysis.

## Public Headers & CMake Target

- Headers:
  - `include/Alias/InclusionBased/ValueFlowPTA/ValueFlowPTA.h`
  - `include/Alias/InclusionBased/ValueFlowPTA/ValueFlowGraph.h`
- Implementation:
  - `lib/Alias/InclusionBased/ValueFlowPTA/ValueFlowPTA.cpp`
  - `lib/Alias/InclusionBased/ValueFlowPTA/ValueFlowGraph.cpp`
- CMake target: `ValueFlowPTA` (alias `Lotus::ValueFlowPTA`)

## Key Mechanisms

- Incremental `ComputePtedSet` reachability and transitive escape-order scheduling
  (`ComputeEspSet`) with batch non-scalar cycle resolution and local scalar-cycle
  weakening (preserving strong updates on unrelated scalars).
- Per-function CFGs with Algorithm 2's callsite-scoped `PreprocessGlobalFlow`
  auxiliary call-in/entry/exit/call-out wiring and block-level sparse
  `ComputeLocalFlow` (with per-function incremental reuse, SCC-condensed weak fast
  path, and factored join nodes).
- Dynamic indirect-call resolution inside the VFG solver with callsite-anchored
  intra-block event insertion for indirect external and `byval` targets, mixed
  defined/external callsite soundness, and address-taken filtering.
- Pointer-only memory modeling, `AliasSpecManager` library summaries (allocators,
  deallocators, copy/return-alias/no-effect specs), `byval` value transfer, full
  vs. partial `memcpy`/`memmove`/`memset`, and priority-ordered
  `llvm.global_ctors`/`llvm.global_dtors` lowering.
- Public VFG inspection APIs (`getProgram`, `getSolver`, `getValueNode`,
  `getStoreNode`, `getPointedToByStores`, `getVFGSuccessors`,
  `getVFGIndirectSuccessors`) for downstream value-flow analyses.

## Usage

Run via the flow-sensitive alias driver:

```bash
lotus-alias-fspta input.bc --analysis=vfpta --print-pts
```

Programmatic usage:

```cpp
#include "Alias/InclusionBased/ValueFlowPTA/ValueFlowPTA.h"

lotus::alias::ValueFlowPTA analysis(module);
analysis.analyze();
const auto &pts = analysis.getPointsTo(ptr);
```

## See Also

- Inclusion-based overview: `lib/Alias/InclusionBased/README.md`
- SVFG-based sparse solvers: `lib/Alias/InclusionBased/SparsePTA/README.md`
- Documentation: `docs/source/alias/valueflowpta.rst`
