# Demand-Driven Pointer Analysis (DDA)

Demand-driven pointer analysis computes points-to sets on demand by walking
backward over the Sparse Value-Flow Graph (SVFG), following SVF's
`FlowDDA` / `DDAVFSolver` design (FSE'16, TSE'18).

Unlike exhaustive analyses that solve every pointer up front, DDA only solves
the queried pointer, which is often a better fit for large programs and
query-driven tools.

## What is Demand-Driven Analysis?

**Exhaustive pointer analysis**
```
Input: Entire program
Process: Analyze ALL pointers
Output: Points-to sets for every pointer
Cost: High (analyzes everything)
```

**Demand-driven analysis**
```
Input: Specific pointer query
Process: Backward traverse SVFG from query point
Output: Points-to set for queried pointer only
Cost: Low (analyzes only what's needed)
```

### Example

```c
int x, y, z;
int *p = &x;        // p -> {x}
int *q = &y;        // q -> {y}
int *r = &z;        // r -> {z}
if (cond)
  p = &y;           // p -> {x, y}
int a = *p;         // Query: what does p point to here?
```

Exhaustive analysis computes points-to for `p`, `q`, and `r`; DDA only solves
`p` at the query site.

## Scope In Lotus

Lotus provides two analysis modes and several client types.

### Analysis Modes

- **`FlowDDA`**: Flow-sensitive, context-insensitive. Distinguishes program
  points but merges calling contexts. This is the default and usually the best
  performance/precision tradeoff.

- **`ContextDDA`**: Flow-sensitive, context-sensitive. Distinguishes both
  program points and call chains. It is more precise for recursion and
  callbacks, but slower.

### Client Types

- **`DDAClient`** (`All`): Collects all top-level pointer queries for
  whole-program analysis.

- **`FunptrDDAClient`** (`Funptr`): Collects function pointers at indirect call
  sites for call graph construction and virtual/callback resolution.

- **`AliasDDAClient`** (`Alias`): Collects pointers used by memory operations,
  such as load sources, store destinations, and GEP bases.

### Supporting Components

- **`DDAVFSolver`**: Generic backward solver (CRTP base)
- **`DDAPass`**: Driver that selects the solver mode and client
- **`DPItem`**: Query state during solving

## Core Query Algorithm

Given a queried pointer `p`, DDA computes its points-to set by traversing the
SVFG backward.

### Algorithm Steps

1. **Initialize**: Map `p` to its defining SVFG node and create DPM state
   `(cur, loc)`.
   - `cur`: Current pointer/object node ID
   - `loc`: Current SVFG location (program point)

2. **Backward Traversal**: Run `findPT(dpm)` in `DDAVFSolver`
   - Dispatch by SVFG node kind:
     - **Addr**: Add allocation object to points-to set
     - **Copy/Phi**: Continue backward through operands
     - **Gep**: Adjust field offsets in points-to set
     - **Load**: Get points-to of pointer, then indirect backward traversal
     - **Store**: Apply strong/weak update based on must-alias
     - **Memory nodes**: Handle MU/CHI for memory SSA
   - Traverse direct/indirect incoming value-flow edges
   - Recurse on predecessor DPMs and union the returned points-to sets

3. **Caching**: Store computed points-to sets to avoid recomputation
   - Top-level pointer cache: `dpmToTLPtsMap_`
   - Address-taken/memory cache: `dpmToADPtsMap_`

4. **Refinement**: When new facts appear for a DPM, trigger `reCompute` on
   dependents until a fixpoint is reached.

5. **Termination**: Stop when query reaches fixpoint or hits budget
   - Fixpoint: No new points-to facts discovered
   - Budget: Maximum traversal steps exceeded

### Example Walkthrough

```c
int x, y;
int *p = &x;        // L1: p -> {x}
if (cond)
  p = &y;           // L2: p -> {y}
int z = *p;         // L3: Query p
```

Backward traversal from L3:
```
1. Start: DPM(p, L3)
2. Find phi node merging p from both branches
3. Backward to L2: DPM(p, L2) -> find "p = &y" -> add y to pts
4. Backward to L1: DPM(p, L1) -> find "p = &x" -> add x to pts
5. Result: p -> {x, y}
```

## Usage Examples

### Basic Usage (FlowDDA)

```cpp
#include "Alias/DemandDriven/DDA/FlowDDA.h"

FlowDDA dda;
dda.run(module);

auto pts = dda.getPointsTo(ptr);
for (uint32_t objId : pts) {
    const llvm::Value *obj = dda.getSVFG()->getObjectValue(objId);
    // Process pointed-to object
}

if (dda.mayAlias(ptr1, ptr2)) {
    // ptr1 and ptr2 may alias
}

if (dda.mayNull(ptr)) {
    // ptr may be null
}
```

### Using DDAPass with Client

```cpp
#include "Alias/DemandDriven/DDA/DDAPass.h"

DDAPass dda;

dda.setDDAKind(DDAKind::FlowS_DDA);  // or DDAKind::Cxt_DDA

dda.selectClient(DDAClientKind::Funptr);  // Analyze function pointers

dda.runOnModule(module);

if (dda.mayAlias(ptr1, ptr2)) {
    // ptr1 and ptr2 may alias
}
```

### Custom Query Set

```cpp
#include "Alias/DemandDriven/DDA/DDAPass.h"

DDAPass dda;

dda.addQuery(ptr1);
dda.addQuery(ptr2);
dda.addQuery(ptr3);

// Only the added queries are analyzed
dda.runOnModule(module);
```

### Context-sensitive mode and budget

Switch modes with `dda.setDDAKind(DDAKind::Cxt_DDA)` (FlowDDA is the default); reach the
underlying solver with `dda.getFlowDDA()`. Cap work with
`FlowDDA::setDefaultMaxBudget(10000)` (default 100000) — queries exceeding the budget fall back
to conservative PTA (see "Out-Of-Budget Fallback").

### Custom Client

```cpp
#include "Alias/DemandDriven/DDA/DDAClient.h"
#include "Alias/DemandDriven/DDA/FlowDDA.h"

class MyClient : public DDAClient {
public:
    std::vector<const llvm::Value *> &collectCandidateQueries() override {
        // Collect custom set of pointers
        for (auto &F : *getModule()) {
            for (auto &BB : F) {
                for (auto &I : BB) {
                    if (/* custom condition */) {
                        addCandidate(&I);
                    }
                }
            }
        }
        return candidateQueries_;
    }

    void handleStatement(const SVFGNode *node, uint32_t curNodeId) override {
        // Hook into backward traversal
        // Called for each visited SVFG node
    }

    void performStat(FlowDDA *dda) override {
        // Collect custom statistics
    }
};

// Use custom client
MyClient client;
FlowDDA dda;
dda.setClient(&client);
dda.run(module);
dda.answerQueries();
```

## Out-Of-Budget Fallback

Lotus follows SVF's design of conservative fallback when a demand query exceeds
the step budget:

- Primary fallback source: pointer-analysis object sets from `SVFGBuilder`
  (`AserPTA`-backed in Lotus).
- Candidate fallback values are collected from:
  - query-location value (`dpm.getLoc()->getValue()`), and
  - object-mapped value (`svfg->getObjectValue(dpm.getCurNodeID())`).
- If no object ID can be recovered, fallback inserts the SVFG unknown object.

This guarantees a conservative result when the precise query runs out of budget,
while still preferring PTA-derived object IDs when available.

## Context Sensitivity

`ContextDDA` extends the same transfer logic with call-string constraints: call/return edges
update or match context IDs; recursive callsites are treated context-insensitively where needed;
the context-insensitive edge set is initialized from recursion/value-flow cycles; out-of-budget
downgrades to conservative object-level fallback.

| Aspect | FlowDDA | ContextDDA |
|--------|---------|------------|
| Flow-sensitivity | ✓ | ✓ |
| Context-sensitivity | ✗ | ✓ |
| Precision | Moderate | High |
| Performance | Fast | Slower |
| Memory usage | Low | Higher |
| Best for | General use | Recursive/callback code |

## Performance Characteristics

- **Query time**: O(k) where k = number of SVFG nodes visited
- **Memory**: O(n) where n = number of cached DPMs
- **Budget control**: Limits worst-case query time
- **Caching**: Amortizes cost across multiple queries

### Optimization Tips

1. Set an appropriate budget with `FlowDDA::setDefaultMaxBudget()` (lower for faster queries).
2. Use targeted clients — `selectClient(DDAClientKind::Funptr)` is faster than `All`.
3. Batch queries — calling `getPointsTo` on several pointers lets later queries reuse caches.
4. Pick the right mode — `setDDAKind(DDAKind::FlowS_DDA)` is faster.

## SVFG Assumptions And Invariants

The implementation assumes:

- Each pointer query can be mapped to an SVFG def node.
- Direct and indirect SVFG edge classes are consistent with `isDirectVFGEdge`
  and `isIndirectVFGEdge`, plus Lotus-specific extensions.
- Indirect edge guards (`edge->getPointsTo()`) represent object-sensitive flow.
- Constant/immutable objects are identified (`svfg->isConstantObject`) and
  skipped during indirect memory propagation.

## Key Files

- `include/Alias/DemandDriven/DDA/DDAVFSolver.h`: generic solver algorithm and caches.
- `include/Alias/DemandDriven/DDA/FlowDDA.h`, `lib/Alias/DemandDriven/DDA/FlowDDA.cpp`: flow-sensitive mode.
- `include/Alias/DemandDriven/DDA/ContextDDA.h`, `lib/Alias/DemandDriven/DDA/ContextDDA.cpp`: context mode.
- `include/Alias/DemandDriven/DDA/DDAClient.h`: query clients and candidate collection.
- `include/Alias/DemandDriven/DDA/DDAPass.h`, `lib/Alias/DemandDriven/DDA/DDAPass.cpp`: orchestration.
- `include/Alias/DemandDriven/DDA/DDAStat.h`, `lib/Alias/DemandDriven/DDA/DDAStat.cpp`: solver statistics.


## References

- Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow Refinement". FSE'16.
- Yulei Sui, Jingling Xue. "Value-Flow-Based Demand-Driven Pointer Analysis for C and C++". TSE'18.
