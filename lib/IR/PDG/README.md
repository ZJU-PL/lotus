# PDG: Program Dependence Graph

The **Program Dependence Graph (PDG)** is a fine-grained representation of data
and control dependences built directly from LLVM IR. It supports slicing,
security analyses, and other dependence-aware queries.

The PDG is field-sensitive and flow-insensitive. The query layer exposes
both context-insensitive and call/return-matched context-sensitive traversals
through focused ``*Query`` headers under ``include/IR/PDG/Query/``, with
``Query.h`` as the umbrella include.
The Cypher query-language parser and executor live separately under
``include/IR/PDG/Cypher/`` and ``lib/IR/PDG/Cypher/``.

## Organization and dependencies

Headers and implementations use eight flat directories matching the eight CMake
targets 1:1. Public headers live under `include/IR/PDG/`; internal query helpers
(`QuerySupport.h`, `Traversal.h`) live privately under `lib/IR/PDG/Query/`.

| Directory | Responsibility | CMake target |
| --- | --- | --- |
| `Core/` | Graph representation, parameter trees, wrappers, core utilities, debug info, options, query diagnostics | `CanaryPDGCore` |
| `Construction/` | Structural builder, LLVM dependence passes, call graph | `CanaryPDGConstruction` |
| `Query/` | PDG graph queries: criteria, traversal, slicing, paths, summaries, impact, and diff | `CanaryPDGQuery` |
| `Models/` | Shared finding/rule types, LLVM facts, and C/C++ library models | `CanaryPDGModels` |
| `Analyses/` | LLVM IR domain analyses (bounds, lifetime, state, taint, arithmetic, API, contracts, protocols) | `CanaryPDGAnalyses` |
| `Rules/` | Rule catalog and rule engine dispatch | `CanaryPDGRules` |
| `Cypher/` | Cypher AST, parser, and executor | `CanaryPDGCypher` |
| `Visualization/` | LLVM `GraphTraits` and DOT printer pass | `CanaryPDGVisualization` |

Directory and target dependencies form a strict DAG:

```text
Core          --> LLVM
Construction  --> Core, alias analysis, control dependence
Query         --> Core, LLVMAnalysis
Models        --> LLVM
Analyses      --> Core, Models
Rules         --> Query, Analyses
Cypher        --> Query, Analyses
Visualization --> Construction
```

Link only the targets a consumer uses. There is no aggregate `CanaryPDG`
library or forwarding headers. Graph queries do not depend on the parser,
IR analyses, or rule catalog. `Query/Query.h` includes only graph queries;
include `Models/`, `Analyses/`, and `Rules/RuleEngine.h` explicitly.
`Core/QueryDiagnostics.h` holds shared query diagnostics used by both graph
queries and IR analyses. `Query/QueryTypes.h` contains shared graph-query
vocabulary; criteria, resource types, and transform/diff/dataflow-specific
results have focused headers. `Models/FindingTypes.h` defines the shared
`AnalysisRuleDescriptor` and `AnalysisFinding` types used by all IR analyses.
`QuerySupport.h` and `Traversal.h` are private helpers in `lib/IR/PDG/Query/`
(`query_detail` namespace).

`ProgramGraph` owns nodes, edges, function/call wrappers, and tree wrappers.
LLVM values and debug metadata remain owned by the module. Construct a local
graph with `GraphBuilder(graph).build(module)` and optionally annotate it with
`bindDebugInfo(module)`. Rebuilding replaces its contents and advances the
query-cache epoch; previously returned nodes, edges, and query results must
be discarded. Each data-dependence pass starts a fresh construction cycle,
including after changes to the same module. The legacy LLVM dependence passes use the shared
`ProgramGraph::getInstance()`; structural builders and queries also support
independent graph instances. Call graphs are owned by the pass or caller that
constructs them, with no process-global cached module state.

Cypher is a front end: execute a selector, then pass its nodes through
`PDGCriteria::nodeSet(...)` to graph queries or rules. This factory preserves
an explicitly empty selection instead of treating it as omitted criteria.
Invalid selectors are diagnosed before an analysis runs. The unused,
unbuilt advanced-extension API has been removed; supported Cypher behavior
is defined by the parser and executor compiled into `CanaryPDGCypher`.

## Key Features

- **Data Dependencies**: Def-use chains, read-after-write (RAW), and alias-based memory dependencies
- **Control Dependencies**: Execution order and branch condition dependencies
- **Interprocedural**: Tracks dependencies across function boundaries
- **Field-Sensitive**: Handles structure fields and array elements separately
- **Parameter Trees**: Tree structures for field-sensitive parameter analysis

## Graph queries and semantic services

- **`SliceQuery`**: forward slices, backward slices, chops, and thin slices
- **`DependenceQuery`**: reachability, shortest paths, all shortest paths, and
  distance queries
- **`DataFlowQuery`**: reaching definitions, def-use/use-def chains, liveness,
  and control-region queries
- **`TransformQuery`**: motion legality and dependence-aware scheduling helpers
- **`DiffQuery`**: structural PDG differencing and impact summaries
- **`ImpactQuery`**: ranked impact report for a changed node, with shortest
  distance and interprocedural-crossing metadata
- **`SummaryQuery`**: source/target relationship buckets with witness paths
- **`ResourceFlowQuery`**: acquire/use/transfer/release events over resource
  families
- **`RuleEngine`**: semantic CodeQL migrations with IDs, coverage boundaries,
  findings, CWE tags, and evidence; shared LLVM facts live in **`ValueFacts`**
- **`BoundsAnalysis`**: allocation/subobject capacity, access widths and offsets,
  symbolic end relations, program-point string termination/length and format
  output bounds; uses LLVM AA and MemorySSA
- **`LifetimeAnalysis`**: per-object CFG release/escape states, matched direct
  calls, realloc alternatives and lock counts; compatible branch predicates
  distinguish double release, use after release and never/may resource leaks
- **`StateAnalysis`**: strong local updates, guard-consistent path witnesses,
  null/dereference and conditional output summaries, scanf output effects;
  exposes pre-access pointer/initialization facts
- **`TaintAnalysis`**: PDG-owned CFG worklist and context-keyed function summaries,
  matched call/return mappings, field offsets, library transfers,
  parameter-provenance wrapper summaries, flag guards and domain-specific barriers; exposes source and
  concatenation evidence without interpreting arbitrary PDG edges as taint
- **`ArithmeticAnalysis`**: promotion/bitwidth-aware mathematical ranges, edge
  guards, MemorySSA definitions, allocation provenance and scalar taint
- **`ApiAnalysis`**: security settings and CFG API protocols for certificates,
  cryptography, XML entities, DACLs and permission masks
- **`CallContractAnalysis`**: target-ABI format types, argument contracts and
  inferred variadic sentinel conventions
- **`ProtocolAnalysis`**: bounded shared-global race and lock-order candidates
  across potentially overlapping pthread workers, plus stable-path filesystem
  check/use protocols. These are Lotus-specific `lotus/` rules with explicit
  modeling limits, separate from the migrated `cpp/` IDs.
- **`CppLibraryModels`**: exact standard string API effects with separate opaque
  object contents and bytes; stable object-return aliases and numeric barriers
- **`PDGCriteriaResolver`**: criteria resolution from nodes, LLVM values,
  function names, callee names, source locations, and property specs

Graph traversal services consume a common option/result vocabulary. Semantic
analyses expose their own typed facts/results and feed rule evaluation:

- ``PDGQueryOptions``: edge preset, scope, context mode, limits, cache policy,
  and explanation mode
- ``PDGQueryScope``: whole graph, explicit node set, function, or prior result
- ``PDGQueryResult``: nodes, induced edges, predecessor map, witness paths,
  distances, and diagnostics

## Usage

```cpp
#include "IR/PDG/Query/SliceQuery.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "llvm/IR/LegacyPassManager.h"

// Run PDG passes
llvm::legacy::PassManager PM;
PM.add(new pdg::DataDependencyGraph());
PM.add(new pdg::ControlDependencyGraph());
auto *pdgPass = new pdg::ProgramDependencyGraph();
PM.add(pdgPass);
PM.run(module);

pdg::ProgramGraph *G = pdgPass->getPDG();
pdg::SliceQuery slicer(*G);

pdg::PDGCriteria criteria;
criteria.values.push_back(src);

pdg::PDGQueryOptions options;
options.edge_preset = pdg::PDGEdgePreset::Data;
options.context_mode = pdg::PDGContextMode::ContextSensitive;

auto result = slicer.forward(criteria, options, &module);
if (!result.nodes.empty()) {
  // Criteria reaches at least one node through the selected dependence edges.
}
```

## Edge Presets

The public query API uses fixed edge presets instead of raw edge-type sets:

- ``All``
- ``Data``
- ``Control``
- ``Parameter``
- ``Interprocedural``
- ``ValueFlow``
- ``TransformLegality``

Thin slicing is selected with ``options.slice_flavor = SliceFlavor::Thin``.

## Alias Analysis Selection

PDG data-dependence construction supports alias analysis selection:

- `-pdg-aa=<type>`: Selects over-approximate AA (default: `andersen`)
- `-pdg-aa-under=<type>`: Optionally enables under-approximate AA (default: `underapprox`)

Supported AA types: `andersen`, `andersen-1cfa`, `andersen-2cfa`, `dyck`, `cfl-anders`, `cfl-steens`, `combined`, `underapprox`

## Available Passes

- `-pdg`: Generate the program dependence graph (inter-procedural)
- `-cdg`: Generate the control dependence graph (intra-procedural)
- `-ddg`: Generate the data dependence graph (intra-procedural)
- `-dot-*`: Visualization passes (dot format)

## `lotus-ir-pdg-query`

``tools/ir/lotus-ir-pdg-query.cpp`` supports both raw Cypher queries and analysis mode.

Examples:

- ``lotus-ir-pdg-query --analysis slice-backward --criteria-query "MATCH (n:INST_RET) RETURN n" foo.bc``
- ``lotus-ir-pdg-query --analysis chop --criteria-query "MATCH (a) WHERE a.func = 'main' RETURN a" --target-query "MATCH (b:INST_RET) RETURN b" --edge-preset value-flow foo.bc``
- ``lotus-ir-pdg-query --analysis diff --criteria-query "MATCH (n) WHERE n.func = 'old_f' RETURN n" --target-query "MATCH (n) WHERE n.func = 'new_f' RETURN n" --format json foo.bc``
- ``lotus-ir-pdg-query --property-file spec.prp --direction backward --context-sensitive foo.bc``

Analysis mode accepts ``--scope-function``, ``--scope-query``,
``--context-sensitive``, ``--thin``, and ``--format text|json|dot``.

Semantic rules use `--analysis rules --rule <CodeQL ID> --format text|json`;
`--list-rules` lists rules and coverage. Rules use scope/criteria but do
not infer taint or path feasibility from dependence reachability.
CLI lifetime exploration defaults to 4096 states per object
(`--lifetime-state-limit`, 0 disables this budget), independently of the
`--taint-step-limit` worklist budget. Incomplete exploration is reported.
`--protocol-state-limit` separately controls protocol worker/check/cycle
searches (default 4096, zero disables the budget).
Each rule descriptor records retained-IR and source-information limitations.
Shared callee resolution follows immutable function-pointer globals, unchanged
local spills and agreeing select/phi targets. Bounds checks also use guarded
SCEV index ranges for stack/global arrays, and lifetime checks model local
`unique_ptr` default-deleter cleanup/get/release/reset effects. See
`tools/ir/examples/README.md` and `COVERAGE.md` for the capability boundaries
and the catalog-to-example mapping.

## See Also

- Headers: `include/IR/PDG/`
- Documentation: `docs/source/ir/pdg.rst`
- Query API: `include/IR/PDG/Query/Query.h`
- Focused slicing API: `include/IR/PDG/Query/SliceQuery.h`
- Cypher query language: `include/IR/PDG/Cypher/Cypher.h`
