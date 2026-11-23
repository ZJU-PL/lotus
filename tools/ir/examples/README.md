# PDG Cypher query examples

These files are meant to be run with `lotus-ir-pdg-query`. Queries may span multiple lines and are separated by blank lines, `#` comment lines, a trailing `;`, or the start of a new `MATCH` after a completed query; `#` starts a comment.

## Run

- Build: `cmake --build build --target lotus-ir-pdg-query`
- Execute a query file: `./build/bin/lotus-ir-pdg-query -f tools/ir/examples/metrics.cypher <input.bc>`
- New primitives demo: `./build/bin/lotus-ir-pdg-query -f tools/ir/examples/primitives.cypher <input.bc>`

For metric-style queries where you want the full count, run with `--limit 0` (otherwise `Result(N nodes)` reflects the limited result set).

By default, `lotus-ir-pdg-query` runs the `ProgramDependencyGraph` pass (`--build-pdg`) so edge queries (data/control/param/call) work.

## PDG schema introspection

Instead of relying on this mini schema, get the complete up-to-date schema as JSON:

```bash
./build/bin/lotus-ir-pdg-query --schema
```

This prints all node labels, edge types, node/edge properties, group labels, and edge presets. LLM agents should call this before writing Cypher queries.

## Mini schema (Lotus PDG)

**Labels**
- `:INST` (all instruction nodes), `:INST_FUNCALL`, `:INST_RET`, `:INST_BR`, `:INST_OTHER`
- `:FUNC_ENTRY`
- `:PARAM` (all parameter-tree nodes), `:PARAM_FORMALIN`, `:PARAM_FORMALOUT`, `:PARAM_ACTUALIN`, `:PARAM_ACTUALOUT`
- `:VAR` (all variable nodes; includes globals), plus the specific `:VAR_*` labels
- `:ANNO` (annotation nodes), plus `:ANNO_VAR`, `:ANNO_GLOBAL`, `:ANNO_OTHER`

**Edges (for `-[:TYPE]->`)**
- `:DATA_DEP` (def-use), `:DATA_RAW`, `:DATA_READ`, `:DATA_ALIAS`
- `:CONTROL_DEP`
- `:CALL_INV`, `:CALL_RET`, `:IND_CALL`
- `:PARAM_IN`, `:PARAM_OUT`

## Extended query primitives

- **Directional traversals:** `-[:T]->` follows outgoing edges; `<-[:T]-` follows incoming edges; `-[:T]-` or `<-[:T]->` follows both.
- **Multi-edge-type traversals:** `:T1|T2` matches either edge type (e.g., `:DATA_DEP|CONTROL_DEP`).
- **List filters:** `WHERE n.opcode IN ["load","store"]`.
- **Aggregation:** `RETURN COUNT(*)`, `COUNT(n)`, `COUNT(DISTINCT n.prop)`.
- **Parameters:** pass `--param key=value` and reference as `$key` (works in `{prop:$key}` and `WHERE`).

**Common node properties (usable in `WHERE` and `RETURN`)**
- `func`, `label`
- `opcode` (LLVM opcode for instruction nodes)
- `callee` (for `:INST_FUNCALL`; returns `"<indirect>"` for indirect calls)
- `arg_count`, `callee_param_count`, `arg<N>_int`, `arg<N>_object_bytes`
  (argument indices start at zero; unknown facts are empty)
- `src`, `src_file`, `src_line`, `src_col` (requires debug info in the input bitcode)
- `known_access_count`, `out_of_bounds_access_count`, `read_out_of_bounds`,
  `write_out_of_bounds` (known object/subobject accesses; absence of a proven
  overflow does not prove unknown accesses safe)
- `access_bytes`, `access_capacity_bytes`, `access_offset_bytes` (exactly one
  known access; sizes follow the target ABI and offsets can be negative)
- `arg<N>_string_termination`, `arg<N>_string_min_bytes`, `arg<N>_string_max_bytes`
  (content before the call, keeping unknown termination distinct)
- `format_output_max_bytes`, `format_output_small_float_max_bytes`,
  `format_output_is_buffer`, `format_output_limit_arg` (parsed output including
  the NUL and API destination/limit roles)
- `pointer_nullness`, `pointee_initialization` (load/store address and pointee
  immediately before the instruction; applicable unknown facts are `unknown`)
- `llvm` (LLVM IR string for the underlying value/instruction)

**Common edge properties**
- `label`, `src_*`, `dst_*` (e.g., `e.label`, `e.src_func`, `e.dst_src`)

---

## Security queries

The `security/` directory contains categorized security analysis patterns.
[COVERAGE.md](COVERAGE.md) maps every native rule ID to an example. The catalog
includes both migrated `cpp/` IDs and Lotus-specific `lotus/` protocol IDs;
IDs and CWE tags do not establish complete coverage of a weakness category.

There are three distinct kinds of example:

- API inventories select sites worth inspecting; API presence alone is not a bug.
- Semantic predicates compare modeled facts such as access size and capacity.
  Unknown facts remain unknown; missing findings do not establish safety.
- Native `--analysis rules` commands use CFG, memory, argument roles and object
  state. Dependence paths from chop/slice explain relationships and cannot
  replace taint, lifetime, synchronization or path-feasibility reasoning.

| File | Patterns | Analysis modes |
|------|----------|----------------|
| `static-rules.cypher` | Recursion, stack allocation, conversions, comparisons and return contracts | Inventories + native retained-IR checks |
| `security/codeql-arithmetic.cypher` | Promotions, overflow, truncation, division and tainted arithmetic | Inventories + native range/MemorySSA checks |
| `security/codeql-api-protocols.cypher` | TLS/certificates, cryptography, XML entities, DACLs and permissions | Inventories + native configuration/CFG protocols |
| `security/codeql-call-contracts.cypher` | Format ABI types, signatures and variadic sentinels | Inventories + native ABI/contract checks |
| `security/protocols.cypher` | Shared-global races, lock-order cycles and filesystem TOCTOU candidates | Inventories + bounded native protocol checks |
| `security/codeql-taint.cypher` | Shared source/sink roles for PDG-native process, command, SQL, path and format flows | Cypher selections + `--analysis rules`; source/concat evidence in JSON |
| `security/codeql-string-bounds.cypher` | Program-point string termination/length, buffer formatting roles and output bounds | Shared MemorySSA string/format facts; native rules for taint and relative guards |
| `security/codeql-memory-access.cypher` | Subobject capacity, access width, negative offsets, read/write overflow | Shared BoundsQuery facts; native rules also check symbolic ends |
| `security/codeql-memory-state.cypher` | Nullable dereferences, definite/conditional uninitialized loads | Shared StateQuery facts; native lifetime rules retain object/path state |
| `security/codeql-buffer-bounds.cypher` | Target-ABI copy bounds versus destination capacity | Semantic Cypher predicates; native rules for dynamic lengths |
| `security/codeql-format-arguments.cypher` | Parsed format counts and unbounded scanf conversions | Semantic Cypher predicates |
| `security/codeql-resource-origins.cypher` | Allocation/release families and value-flow explanations | Cypher + native family checks/context-sensitive chop |
| `security/codeql-patterns.cypher` | Semantic call/argument facts for CodeQL migrations | Cypher inspection; `--analysis rules` for supported checks |
| `security/injection.cypher` | Process, shell-command and wordexp sink roles | Inventories + native domain-specific taint checks |
| `security/memory.cypher` | UAF, double-free, never/may leaks and realloc failure | Inventories + native lifetime checks |
| `security/unsafe-libc.cypher` | Copy/format argument roles and unsafe APIs | Inventories + native bounds/format checks |
| `security/resource.cypher` | File/descriptor leaks, release and mutex protocols | Inventories + native lifetime checks; mmap exploration |
| `security/double-free.cypher` | Double-free, UAF, C++ ownership and allocation families | Shared-pointer candidates + native object/CFG checks |
| `security/taint.cypher` | Input-to-sink content flow, overwrites and format roles | Inventories + native taint summaries |

Each file documents the prerequisites and CLI invocation for each pattern.

`--list-rules` lists supported rule IDs and their coverage. Run
`--analysis rules --rule cpp/bad-strncpy-size --format json` for semantic
checks. `--list-cwes` groups implemented IDs by original CodeQL CWE tags;
`--analysis rules --cwe 401,415,416,457,476,775 --format json` selects relevant
rules. These tags describe partial category coverage, not complete CWE coverage.
Dependence paths in the examples are
candidates for inspection, not proofs of taint or feasible resource misuse.

The semantic examples support property-to-property comparisons such as
`c.copy_size_bytes > c.copy_destination_bytes`. Models supply argument roles,
target ABI sizes and parsed format facts, so these are checks over program
semantics rather than spelling or coding style. BoundsQuery, StateQuery and
LifetimeQuery share reusable analysis services; none depends on IFDS, Saber,
Pulse or UseTraceSSA. Further work prioritizes interprocedural object identity,
ownership summaries, relative ranges and C++ library models.

### Quick-start security triage

```bash
# 1. Find all shell execution call sites
./build/bin/lotus-ir-pdg-query input.bc -f tools/ir/examples/security/injection.cypher

# 2. Check heap lifetime and realloc failure
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis rules --cwe 401,415,416 \
  --format json

# 3. Check file/descriptor lifetimes
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis rules --cwe 775 \
  --format json

# 4. Find strcpy/gets call sites
./build/bin/lotus-ir-pdg-query input.bc -f tools/ir/examples/security/unsafe-libc.cypher
```

### Source-sink data flow (chop analysis)

For exploring dependence paths from input-like calls to sensitive calls:

```bash
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis chop \
  --criteria-query "MATCH (s:INST_FUNCALL) WHERE s.callee IN ['fgets','read','scanf','getenv','recv'] RETURN s" \
  --target-query "MATCH (t:INST_FUNCALL) WHERE t.callee IN ['system','popen','execve'] RETURN t" \
  --edge-preset value-flow \
  --context-sensitive \
  --format json
```

The `witness_paths` field explains graph connectivity. This does not establish
input content flow, effective sanitization, or a feasible execution. Use
`--analysis rules --rule cpp/uncontrolled-process-operation` for modeled taint.

## Semantic coverage and remaining boundaries

| Area | Implemented extension | Remaining boundary |
| --- | --- | --- |
| Indirect calls | Unique immutable pointer globals, unmodified local spills and agreeing select/phi targets share native callee resolution | Mutable/ambiguous targets, virtual dispatch and general points-to sets |
| Dynamic bounds | SCEV and dominating guards prove all possible indices of a two-index GEP outside a stack/global array | Partially overlapping ranges, multidimensional dynamic subobjects and cross-call capacities |
| C++ ownership | Local `unique_ptr` with `default_delete`: construction, get/release/reset and destructor effects | Moves, custom deleters, shared ownership and arbitrary RAII classes |
| Races | Conflicting shared-global byte ranges in potentially overlapping registered pthread workers; atomic and mutex suppression | Heap aliases, parent-thread accesses, helper effects and general happens-before |
| Deadlocks | Global mutex lock-order cycles across potentially overlapping workers | Feasible schedules, recursive POSIX mutex attributes, try-lock protocols and general lock aliases |
| TOCTOU | CFG-connected stable-path access/stat/lstat followed by a separate filesystem use | Symlink resolution, openat-relative/descriptor protocols and general feasibility |
| LLVM input | Retained instructions, target ABI, modeled memory and library effects | Source AST distinctions and optimized-away behavior; use `-g`, and `-fdebug-macro` when flags require it |

The `lotus/` concurrency rules report candidates under successful blocking-lock
models. No findings does not prove race freedom or deadlock freedom. Native
protocol exploration is bounded; budget exhaustion is reported in diagnostics.
`--protocol-state-limit` controls the per-worker/check/cycle budget (default
4096 states; zero disables it), independently of taint and lifetime budgets.
The shared callee extension affects native facts and the Cypher `callee`
property. Graph call-edge construction retains its own target-resolution scope.

## Regression fixtures

`fixtures/security.ll` contains unsafe, safe and unknown cases for the actual
Cypher predicates and native lifetime/taint/format checks. Backend regressions
in `tests/unit/IR/PDG/RuleCoverageTest.cpp` cover each extension above, including
clobbered pointers, guarded array indices, ownership release/reset, atomic and
mutex suppression, joined workers, path mutation and exclusive file creation.
The wider native rule suite is in `RuleQueryTest.cpp`. These are regression
fixtures; they do not measure real-world precision or recall.

```bash
cmake --build build --target lotus-ir-pdg-query ir_tests
python3 tests/regress/IR/PDG/check_examples.py
build/bin/tests/ir_tests --gtest_filter='PDGRuleQueryTest.*:PDGRuleCoverageTest.*'
ctest --test-dir build -R '^pdg_example_queries$' --output-on-failure

# Regenerate the catalog-to-example matrix after adding a rule/example.
python3 tests/regress/IR/PDG/check_examples.py --write-coverage
```

The checker executes every query file, checks positive and negative predicate
results and native findings, and requires a current matrix for every catalog ID.
Batch parse/execution errors and missing files return a nonzero exit code.
