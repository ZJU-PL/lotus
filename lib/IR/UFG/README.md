# Object-expanded UFG baseline

`lotus::ufg::UFGGraph` is the object-expanded use-flow graph baseline for the
UseTraceSSA resource clients. It consumes the same `TraceFlowGraph` produced by
the Lotus SVFG importer. No pointer analysis or library model is rerun.
The existing object-expanded construction is unchanged; it is a projection of
UseTraceSSA's temporal graph, not a direct implementation of SMOKE's sparse
UFG construction rules.

Construction enumerates the finite object universe from upstream guards (plus
optional caller-supplied IDs). For every object, it copies the temporal graph's
nodes and eligible edges into a disconnected lane. Guarded node effects become
ordinary events in that lane, with the same certainty as the fixed-object
UseTraceSSA projection. Known-empty guards create no lane edge; unknown guards
apply to every lane. The `UnknownResource` sentinel gets its own lane.
Flow-node IDs, flow-edge IDs, witness IDs, and UFG search/proof IDs are 64-bit.
Expansion size checks use checked 64-bit arithmetic. This removes the 32-bit
identifier ceiling while preserving eager materialization and its memory cost.

`UFGGraph::runObject` accepts a UseTraceSSA `Query` with original node IDs and
runs `lotus::ufg::SearchEngine` on one materialized lane. Its tabulation keys
are graph node and FSM state; balanced call/return summaries preserve context.
There are no object masks in the search. `lotus::ufg::DefectDetector` exposes
the same `run` and `scan` signatures and result types as the UseTraceSSA
detector. A resource scan searches each object once and collects all accepting
sites from that traversal. Taint and unchecked-use queries run the same UFG
tabulation engine on the source flow graph because they do not require a fixed
object along the path. The UFG detector does not call the UseTraceSSA solver.
Memory-leak and file-leak use the same per-object search, with root exits as
sinks and separate heap/file events. They share the source graph's limited
ownership-escape model.
The lane search reports product states, product edges, and call/return summary
facts. It does not compute or report object-mask operations.
The default is bounded call-string search with depth 3. `--context-limit=k`
(alias `--context-depth=k`) changes the depth. Legacy depth 0 immediately
merges older contexts while still matching the latest call site. Use
`--context=insensitive` to disable matching; `--context=sensitive` is the default.
The independent `--context-limit=unlimited` (alias `--unbounded-context`)
selects unbounded Dyck summaries for context-sensitive search.
Bounded search keeps recent call sites and merges older contexts, so recursion
can add candidates.

Search budgets default to unlimited. Optional `--max-product-states` and
`--max-summary-pairs` accept N or `unlimited` (0 is an alias).
Limits apply to each lane query; they do not limit eager graph construction.
Budget exhaustion preserves existing findings, reports `search_complete=false`
with the first stop reason and counter, and makes the CLI exit with code 2.
`model_complete` independently describes model coverage. Memory exhaustion and
external timeouts remain process failures.

```sh
cmake --build build --target lotus-ir-ufg ir_tests
build/bin/lotus-ir-ufg input.bc --check=double-free --timing
build/bin/lotus-ir-ufg input.bc --check=use-after-free --quiet
build/bin/lotus-ir-ufg input.bc --format=json
ctest --test-dir build -R 'ir_tests.UFG' --output-on-failure
```

The CLI accepts the same `--check`, `--source-node`, `--sink-node`, `--format`,
`--quiet`, `--timing`, and `--dump-svfg` options as `lotus-ir-usetracessa`.
`--object=<ID>` restricts a node query to one lane; without it the tool searches
all lanes. `ufg_objects`, `ufg_nodes`, and `ufg_edges` report the physical
expansion. `shared_temporal` and `ufg_expand` split construction into its two
steps; `ufg` is their sum. Findings remain potential witnesses in the supplied
abstraction; incomplete native models make negative results unknown.
