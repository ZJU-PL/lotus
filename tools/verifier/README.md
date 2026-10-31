# Verifier tools

This directory contains verification-oriented command-line frontends built on
top of `lib/Verification/` and imported verifier integrations.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Several verifier families are optional:

- `lotus-verify-sifa`, `lotus-verify-symabs-ai`, and `lotus-verify-mosaic` are configured by default.
- CLAM tools are built only when `LOTUS_ENABLE_CLAM=ON`.
- SeaHorn tools are built only when `LOTUS_ENABLE_SEAHORN=ON`.
- SMACK frontend (`llvm2bpl`) is built only when `LOTUS_ENABLE_SMACK=ON`.
- `hice-dt` is built only when `LOTUS_ENABLE_HORN_ICE=ON`.
- Boolean-program tools (`lotus-bool-parse`, `lotus-bool-normalize`) are built only when `LOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS=ON`.

## Tool families

| Tool / family | Availability | Purpose |
| --- | --- | --- |
| `lotus-verify-sifa` | default | Symbolic Interpretation with Fluid Abstractions for reachability and invariant inference. |
| `lotus-verify-symabs-ai` | default | Abstract-interpretation driver with configurable domains, fragmentation, and memory models. |
| `lotus-verify-mosaic` | default | Parses Horn SMT-LIB2 files and solves them with Mosaic or Z3's bit-vector engine. |
| `clam`, `clam-diff`, `clam-pp` | `LOTUS_ENABLE_CLAM=ON` | CLAM-based abstract interpretation, preprocessing, and JSON differencing. |
| `seahorn`, `seapp`, `seainspect` | `LOTUS_ENABLE_SEAHORN=ON` | SeaHorn verification, preprocessing, and inspection tools. |
| `llvm2bpl` | `LOTUS_ENABLE_SMACK=ON` | SMACK LLVM-to-Boogie translation and verification frontend. |
| `hice-dt` | `LOTUS_ENABLE_HORN_ICE=ON` | ICE-style learning for Horn clauses / Boogie workflows. |
| `lotus-bool-parse`, `lotus-bool-normalize` | `LOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS=ON` | Parsing and normalizer tools for Boolean/predicate programs. |

## Default tools

### `lotus-verify-sifa`

`lotus-verify-sifa` analyzes a selected function in LLVM bitcode and can run either the fast
instruction-level transfer semantics or the SMT-backed SymbolicAbstraction
backend.

Implementation: `tools/verifier/sifa/lotus-verify-sifa.cpp`

```bash
build/bin/lotus-verify-sifa input.bc --function main --abstract-domain Interval
build/bin/lotus-verify-sifa input.bc --symabs --abstract-domain Octagon --reachability
```

See `tools/verifier/sifa/README.md` for detailed options.

### `lotus-verify-symabs-ai`

`lotus-verify-symabs-ai` is a configurable abstract-interpretation frontend with pluggable
domains and configuration files.

Implementation: `tools/verifier/symabs-ai/lotus-verify-symabs-ai.cpp`

```bash
build/bin/lotus-verify-symabs-ai --list-domains
build/bin/lotus-verify-symabs-ai input.bc --function main --abstract-domain Interval
build/bin/lotus-verify-symabs-ai input.bc --config config/symabs-ai/01_const_function.conf --check-assertions
```

Useful options include `--list-configs`, `--fragment-strategy`,
`--memory-model`, `--widening-delay`, and `--check-memsafety`.

## Optional integrations

- CLAM documentation is in `tools/verifier/clam/README.md`.
- SeaHorn runtime notes are in `tools/verifier/seahorn/sea-rt/README.md`.
- `hice-dt` is the executable built from `third-party/horn-ice/hice-dt/`.

## Related documentation

- `lib/Verification/README.md` documents the verification libraries.
- `config/symabs-ai/README.md` explains SymAbsAI configuration files.
