# IR tools

This directory contains command-line frontends for LLVM-based intermediate
representations built in `lib/IR/`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The current IR tool binary is emitted under `build/bin/`.

## Tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-ir-pdg-query` | Query the Program Dependence Graph | A small entry point with CLI support in `pdg-query/`; supports Cypher queries, slicing, chopping, shortest paths, summaries, resource flow and semantic rules. |
| `lotus-ir-control-dependence` | Run control-dependence experiments | Separates baseline and compact NTSCD/DOD timing, biclique statistics, exact pair enumeration, closure, and consistency checking; emits text, JSON, or CSV. |
| `lotus-ir-usetracessa` | Build UseTraceSSA from Lotus SVFG | Emits text, JSON or DOT; queries node reachability and checks double-free, use-after-free, memory-leak, and file-leak candidates. |
| `lotus-ir-ufg` | Build the object-expanded UFG baseline from the same SVFG facts | Uses the same resource checks and reports physical lane/node/edge counts. |

The PDG query entry point initializes LLVM, loads the module, builds the graph,
and starts the driver. `pdg-query/Options` owns CLI registration and returns a
plain configuration snapshot. `pdg-query/Driver` selects modes, resolves criteria
and invokes PDG analysis services. `pdg-query/Output` renders catalogs, schema,
text, JSON, DOT and Cypher results. Property-driven mode selection uses local
state instead of modifying global CLI options. Analysis implementations and
rule catalogs remain under `lib/IR/PDG/`, so adding a rule does not enlarge the
entry point.

### UseTraceSSA

```bash
build/bin/lotus-ir-usetracessa test.bc --format=json
build/bin/lotus-ir-usetracessa test.bc --format=dot
build/bin/lotus-ir-usetracessa test.bc --source-node=12 --sink-node=34
build/bin/lotus-ir-usetracessa test.bc --check=double-free
build/bin/lotus-ir-usetracessa test.bc --check=use-after-free
build/bin/lotus-ir-usetracessa test.bc --check=use-after-free --context-limit=3
build/bin/lotus-ir-usetracessa test.bc --check=memory-leak
build/bin/lotus-ir-usetracessa test.bc --check=file-leak
```

The tool builds ICFG and an AserPTA-backed SVFG, then derives UseTraceSSA with
shared temporal histories and guarded object effects. Full graph mode also
builds MemorySSA. `--dump-svfg=path.dot` saves the source SVFG. Resource checks
model `malloc`/`calloc` with `free`, `fopen` with `fclose`, and load/store uses.
`Found` is a potential witness in the abstraction; incomplete external models
or cross-function resource effects prevent a negative safety conclusion.

### UFG baseline

```bash
build/bin/lotus-ir-ufg test.bc --check=double-free --timing
build/bin/lotus-ir-ufg test.bc --check=use-after-free --quiet
build/bin/lotus-ir-ufg test.bc --check=use-after-free --context-limit=3
build/bin/lotus-ir-ufg test.bc --check=memory-leak
build/bin/lotus-ir-ufg test.bc --check=file-leak
build/bin/lotus-ir-ufg test.bc --format=json
```

The UFG tool uses the same SVFG importer, pointer facts, and defect rules as
UseTraceSSA. It materializes one temporal graph lane per abstract object and
uses its own tabulation engine to collect all defect sites in one search per
object. See `lib/IR/UFG/README.md` for the API and construction scope.

For batch experiments, `scripts/evaluate_usetracessa.py` runs
UseTraceSSA, UFG, and the checks supported by Saber. It records graph size,
phase timings, peak process RSS, result status, and findings in JSON. Use
`--max-workers 1` when comparing timing and memory; that is the default.
The script passes the same `--context-limit` to all three tools (default 3).
`--context-depth` is an alias. In every tool, k=0 immediately merges older call
context; it does not disable all call/return matching. With only UseTraceSSA/UFG,
use `--context=insensitive` to disable matching. `--context=sensitive` is the
default; the independent `--context-limit=N|unlimited` sets its call-string
depth (`--unbounded-context` aliases `--context-limit=unlimited`). The depth
setting is unused with insensitive context. Both IR tools also default to
depth 3 when invoked directly. Records retain the
system-baseline versus representation-comparison distinction.
The `mode` column describes SMT configuration: UseTraceSSA, UFG, and Saber UAF
all use `nosmt`. Saber UAF remains an object/ICFG reachability candidate,
recorded separately in `analysis_scope`. Memory-leak and file-leak now run in
all three tools; the IR implementations are exit-path candidates with limited
ownership-escape modeling, also recorded in `analysis_scope`.
IR findings group objects by sink, while Saber emits bug reports; the JSON
records each tool's `finding_unit` and the IR object counts separately.

Both IR tools and the evaluation script accept `--max-product-states=N` and
`--max-summary-pairs=N`. Both default to `unlimited`;
numeric 0 is a compatibility alias. Each resource budget is independent of
context settings and the other budgets. These are per-query budgets (one object
batch in UseTraceSSA, one lane in UFG), so identical numbers do not impose
equivalent work across engines. For timing comparisons, use the runner's
`--timeout` and `--mem-limit-gb` with internal budgets unlimited.

```bash
python3 scripts/evaluate_usetracessa.py --tools usetracessa,ufg \
  --benchmarks-dir benchmarks/real-world/SPEC2006 --context-depth 3 \
  --max-product-states 200000 --max-summary-pairs unlimited \
  --results /tmp/spec-budget-eval.json
```

IR output distinguishes `search_complete` from `model_complete`. An exhausted
budget reports its `stop_reason`, `budget_limit`, and `budget_observed`, keeps
any findings already found, and returns exit code 2. The runner records this
as `INCOMPLETE`, independently of `TIMEOUT`/`OOM`; incomplete modeling alone
does not mean the search was interrupted. JSON also records `search_limits`.
Optional `--saber-max-forward-items` and `--saber-solver-timeout-ms` override
Saber-specific budgets; omitting them preserves Saber's defaults.

## Typical usage

`lotus-ir-pdg-query` consumes LLVM bitcode or textual LLVM IR:

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/lotus-ir-pdg-query test.bc --query "MATCH (n) RETURN n LIMIT 5"
```

Useful options include:

- `--query` / `--query-file` to execute Cypher queries.
- `--interactive` to start an interactive query session.
- `--analysis` to run built-in PDG analyses such as `slice-forward`,
  `slice-backward`, `chop`, `shortest-path`, `impact`, or `resource-flow`.
- `--format=text|json|dot` to control output formatting.
- `--property-file` with `--direction` for property-driven slicing.
- `--edge-preset` and `--context-sensitive` to tune traversal behavior.

### Control-dependence experiments

The standalone driver runs exactly one algorithm per invocation and excludes
LLVM parsing/graph construction from `analysis_ns`. Experiment scripts are
responsible for repetitions, warmups, aggregation, and baseline/compact
pairing.

`scripts/evaluate_control_dependence.py` provides that orchestration. It
randomizes baseline/compact run order, performs warmups and repetitions, checks
output counts, and writes `raw.csv`, `summary.csv`, and `metadata.json`.

```bash
# DOD preprocessing only (run as separate script samples).
build/bin/lotus-ir-control-dependence test.bc --algorithm=dod --format=csv
build/bin/lotus-ir-control-dependence test.bc --algorithm=dod-compact --format=csv

# End-to-end preprocessing plus traversal of exactly K pairs. Individual
# pairs are never printed or stored; the callback only increments dod_pairs.
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=dod --visit-pairs --format=csv
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=dod-compact --visit-pairs --format=csv

# Closure comparison. Function entry is always in the seed.
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=strong-closure --seed-index=3 --format=json
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=compact-closure --seed-index=3 --format=json

# Reproducible multi-input evaluation (use the Release driver).
scripts/evaluate_control_dependence.py benchmarks/real-world/SPEC2006 \
  --tool build-release/bin/lotus-ir-control-dependence \
  --experiments ntscd,dod-preprocess,dod-enumerate,combined \
  --warmup 3 --repeat 20 --output-dir results/control-dependence
```

Relevant options:

- `--algorithm=<name>` selects one primitive algorithm operation.
- `--visit-pairs` is valid only for `dod` and `dod-compact`; it traverses exact
  pairs through the same allocation-free counting callback and performs no
  per-pair output.
- `--function=<name>` restricts the experiment to one function.
- `--seed-index=N` adds closure seeds; entry is included automatically.
- `--format=text|json|csv` selects stable machine-readable output.

## Examples

```bash
# Run a single query
build/bin/lotus-ir-pdg-query test.bc --query "MATCH (f:FUNC_ENTRY) RETURN f.name"

# Compute a backward slice from criteria selected by a query
build/bin/lotus-ir-pdg-query test.bc \
  --analysis=slice-backward \
  --criteria-query "MATCH (n {name:'x'}) RETURN n"

# Dump JSON output for scripting
build/bin/lotus-ir-pdg-query test.bc --query-file tools/ir/examples/dataflow.cypher --format=json
```

## Related documentation

- Query examples live in `tools/ir/examples/README.md` and `tools/ir/examples/`.
- PDG implementation details live in `lib/IR/PDG/README.md`.
