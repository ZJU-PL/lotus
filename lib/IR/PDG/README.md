# PDG: Program Dependence Graph

The **Program Dependence Graph (PDG)** is a fine-grained representation of data
and control dependences. It is built on top of the ICFG and is used for
slicing, security analyses, and other dependence-aware queries.

The PDG is field-sensitive and flow-insensitive. The query layer exposes
both context-insensitive and call/return-matched context-sensitive traversals
through focused ``*Query`` headers under ``include/IR/PDG/Analysis/``, with
``Query.h`` as the umbrella include.
The Cypher query-language parser and executor live separately under
``include/IR/PDG/QueryLanguage/`` and ``lib/IR/PDG/QueryLanguage/``.

## Key Features

- **Data Dependencies**: Def-use chains, read-after-write (RAW), and alias-based memory dependencies
- **Control Dependencies**: Execution order and branch condition dependencies
- **Interprocedural**: Tracks dependencies across function boundaries
- **Field-Sensitive**: Handles structure fields and array elements separately
- **Parameter Trees**: Tree structures for field-sensitive parameter analysis

## Query Services

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
- **`RuleQuery`**: semantic CodeQL migrations with IDs, coverage boundaries,
  findings, CWE tags, and evidence; shared LLVM facts live in **`ValueFacts`**
- **`BoundsQuery`**: allocation/subobject capacity, access widths and offsets,
  symbolic end relations, program-point string termination/length and format
  output bounds; uses LLVM AA and MemorySSA
- **`LifetimeQuery`**: per-object CFG release/escape states, matched direct
  calls, realloc alternatives and lock counts; compatible branch predicates
  distinguish double release, use after release and never/may resource leaks
- **`StateQuery`**: strong local updates, guard-consistent path witnesses,
  null/dereference and conditional output summaries, scanf output effects;
  exposes pre-access pointer/initialization facts
- **`TaintQuery`**: PDG-owned CFG worklist and context-keyed function summaries,
  matched call/return mappings, field offsets, library transfers,
  parameter-provenance wrapper summaries, flag guards and domain-specific barriers; exposes source and
  concatenation evidence without interpreting arbitrary PDG edges as taint
- **`ArithmeticQuery`**: promotion/bitwidth-aware mathematical ranges, edge
  guards, MemorySSA definitions, allocation provenance and scalar taint
- **`ApiQuery`**: security settings and CFG API protocols for certificates,
  cryptography, XML entities, DACLs and permission masks
- **`CallContractQuery`**: target-ABI format types, argument contracts and
  inferred variadic sentinel conventions
- **`ProtocolQuery`**: bounded shared-global race and lock-order candidates
  across potentially overlapping pthread workers, plus stable-path filesystem
  check/use protocols. These are Lotus-specific `lotus/` rules with explicit
  modeling limits, separate from the migrated `cpp/` IDs.
- **`CppLibraryModels`**: exact standard string API effects with separate opaque
  object contents and bytes; stable object-return aliases and numeric barriers
- **`PDGCriteriaResolver`**: criteria resolution from nodes, LLVM values,
  function names, callee names, source locations, property specs, and Cypher
  selections

Each service consumes a common option/result vocabulary:

- ``PDGQueryOptions``: edge preset, scope, context mode, limits, cache policy,
  and explanation mode
- ``PDGQueryScope``: whole graph, explicit node set, function, or prior result
- ``PDGQueryResult``: nodes, induced edges, predecessor map, witness paths,
  distances, and diagnostics

## Usage

```cpp
#include "IR/PDG/Analysis/SliceQuery.h"
#include "IR/PDG/Core/ProgramDependencyGraph.h"

// Run PDG passes
legacy::PassManager PM;
PM.add(new DataDependencyGraph());
PM.add(new ControlDependencyGraph());
auto *pdgPass = new ProgramDependencyGraph();
PM.add(pdgPass);
PM.run(module);

ProgramGraph *G = pdgPass->getPDG();
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
- Query API: `include/IR/PDG/Analysis/Query.h`
- Focused slicing API: `include/IR/PDG/Analysis/SliceQuery.h`
- Cypher query language: `include/IR/PDG/QueryLanguage/Cypher.h`
