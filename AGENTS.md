# AGENTS.md — Lotus Program Analysis Framework

## Project Overview

Lotus is a **program analysis, verification, and optimization framework** built on LLVM. It provides alias analysis, intermediate representations, dataflow analysis, abstract interpretation, bug checkers, etc.

- **Language**: C++17
- **Dependencies**: LLVM 14.x, Z3, CMake 3.18+
- **Docs**: https://zju-pl.github.io/lotus

## Repository Layout

```
lotus/
├── include/           # Public headers (mirrors lib structure)
│   ├── Alias/         # Alias analysis (DyckAA, AserPTA, LotusAA, SparrowAA, etc.)
│   ├── Analysis/      # Analysis utilities (NullPointer,CFG, etc.)
│   ├── CFL/           # CFL reachability
│   ├── Checker/       # Bug checkers (AE, Concurrency, FiTx, KINT, Pulse, Saber etc.)
│   ├── Concurrency/   # Concurrency analyses (MHP, lockset, MPI, OpenMP, kernel, CUDA, etc.)
│   ├── Dataflow/      # APA, IFDS/IDE, Mono, NPA, VASCO, WPDS
│   ├── IR/            # GSA, GVFG, ICFG, PDG, SSI, SVFG, vSSA, etc.
│   ├── Solvers/       # Datalog, EGraph, SMT
│   ├── Transform/     # Bitcode transformations
│   ├── Utils/         # LLVM utilities, ThreadPool, formats, etc.
│   └── Verification/  # SIFA, CLAM, smarck, Seahorn, etc.
├── lib/               # Implementations (mirrors include)
├── tools/             # Command-line tools (alias, checker, verifier, ir, etc.)
├── tests/             # GTest-based tests (tests/unit/ mirrors subsystems)
├── benchmarks/        # Benchmark programs
├── third-party/       # CUDD, WPDS, spdlog, etc.
├── scripts/           # Python utilities
└── docs/              # Sphinx documentation (source/)
```

## Build System

- **CMake**: Root `CMakeLists.txt` configures LLVM, Z3, optional Boost
- **Libraries**: Static libs prefixed `Canary*` (e.g., `CanaryDyckAA`, `CanaryPDG`) — legacy naming
- **Tools**: Binaries go to `build/bin/` (e.g., `lotus-alias-lotus-aa`, `lotus-check`, `clam`)
- **Tests**: Unit tests are organized under `tests/unit/`; shared unit-test CMake helpers live in `tests/unit/UnitTestHelpers.cmake`

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
make test
```

Custom LLVM path: `cmake .. -DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm`


## Architecture

```
Tools (lotus-alias-aser-aa, lotus-alias-dyck-aa, lotus-check, clam, etc.)
    ↓
Analysis Applications (Checkers, Optimization, Verification, etc.)
    ↓
Core: Alias Analysis | IR  | Dataflow  Analysis | Abstract Interpretation | etc.
    ↓
LLVM (Module, Function, BasicBlock, Instruction) | Solvers
```


## Testing

- Tests live under `tests/unit/` and are grouped by subsystem (`Alias`, `Analysis`, `CFL`, `Checker`, `Concurrency`, `Dataflow`, `Fuzzing`, `IR`, `Security`, `Solvers`, `SymbolicExecution`, `Transform`, `Utils`, `Verification`).
- Shared unit-test build helpers are defined in `tests/unit/UnitTestHelpers.cmake`, which is included by `tests/unit/CMakeLists.txt`.
- Add new tests using `lotus_add_test_group` or `add_lotus_test_suite` from `tests/unit/UnitTestHelpers.cmake`.
- Shared test support targets include `lotus_test_utils` and `lotus_test_harness_utils`; prefer them over reintroducing large catch-all link bundles.
- Run all tests with `cd build && ctest --output-on-failure`, or build specific test targets with `cmake --build build --target <test_name>`.
