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

| File | Patterns | Analysis modes |
|------|----------|----------------|
| `security/codeql-taint.cypher` | Shared source/sink roles for PDG-native process, command, SQL, path and format flows | Cypher selections + `--analysis rules`; source/concat evidence in JSON |
| `security/codeql-string-bounds.cypher` | Program-point string termination/length, buffer formatting roles and output bounds | Shared MemorySSA string/format facts; native rules for taint and relative guards |
| `security/codeql-memory-access.cypher` | Subobject capacity, access width, negative offsets, read/write overflow | Shared BoundsQuery facts; native rules also check symbolic ends |
| `security/codeql-memory-state.cypher` | Nullable dereferences, definite/conditional uninitialized loads | Shared StateQuery facts; native lifetime rules retain object/path state |
| `security/codeql-buffer-bounds.cypher` | Target-ABI copy bounds versus destination capacity | Semantic Cypher predicates; native rules for dynamic lengths |
| `security/codeql-format-arguments.cypher` | Parsed format counts and unbounded scanf conversions | Semantic Cypher predicates |
| `security/codeql-resource-origins.cypher` | Allocation/release families and value-flow explanations | Cypher + native family checks/context-sensitive chop |
| `security/codeql-patterns.cypher` | Semantic call/argument facts for CodeQL migrations | Cypher inspection; `--analysis rules` for supported checks |
| `security/injection.cypher` | Command injection: system/popen/exec sinks + input source tracing | Cypher API scan + `--analysis chop` |
| `security/memory.cypher` | Use-after-free, double-free, memory leaks | `--analysis resource-flow`, `--analysis chop`, `--analysis shortest-path` |
| `security/unsafe-libc.cypher` | strcpy/gets/sprintf, format string, buffer overflow | Cypher API scan + `--analysis chop` + backward slice |
| `security/resource.cypher` | File handle leaks, lock/unlock, mmap/munmap | `--analysis resource-flow`, `--analysis summary` |
| `security/double-free.cypher` | Double-free, use-after-free, new/delete mismatch | `--analysis resource-flow`, `--analysis chop`, `--analysis shortest-path` |
| `security/taint.cypher` | Input-to-sink taint tracking, format string, argument tracing | Cypher API scan + `--analysis chop` + `--analysis slice-backward` |

Each file documents the prerequisites and CLI invocation for each pattern.

`--list-rules` lists supported CodeQL rule IDs and their coverage. Run
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

# 2. Detect heap memory leaks (malloc without free)
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis resource-flow \
  --criteria-query "MATCH (c:INST_FUNCALL) WHERE c.callee IN ['malloc','calloc','realloc'] RETURN c" \
  --resource-kind heap \
  --format json

# 3. Detect file handle leaks (fopen without fclose)
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis resource-flow \
  --criteria-query "MATCH (c:INST_FUNCALL) WHERE c.callee = 'fopen' RETURN c" \
  --resource-kind file \
  --format json

# 4. Find strcpy/gets call sites
./build/bin/lotus-ir-pdg-query input.bc -f tools/ir/examples/security/unsafe-libc.cypher
```

### Source-sink data flow (chop analysis)

For tracing data flow from input sources to security-sensitive sinks:

```bash
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis chop \
  --criteria-query "MATCH (s:INST_FUNCALL) WHERE s.callee IN ['fgets','read','scanf','getenv','recv'] RETURN s" \
  --target-query "MATCH (t:INST_FUNCALL) WHERE t.callee IN ['system','popen','execve'] RETURN t" \
  --edge-preset value-flow \
  --context-sensitive \
  --format json
```

The `witness_paths` field in the JSON output shows the instruction chain
connecting source to sink.
