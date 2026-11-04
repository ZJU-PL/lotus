# Lotus Unit Tests

`tests/unit` contains subsystem-level GTests for Lotus. Its structure follows
the production taxonomy so contributors can move between `include/`, `lib/`,
and `tests/unit/` without translating historical names.

See [the test guide](../README.md) for placement rules, execution labels, CLI
regressions, and workflow tests. The `unit/` path identifies GTest sources;
individual cases may carry `unit`, `component`, or `integration` labels.

## Layout

Top-level test buckets:

- `Alias/` for alias-analysis and pointer-analysis tests
- `Analysis/` for general analyses, with subdirectories such as `CFG/`,
  `DebugInfo/`, `Loop/`, `NullPointer/`, `Profile/`, and `Purity/`
- `CFL/` with subdirectories `Classical/`, `CSIndex/`, `DynamicDyck/`, and
  `InterleavedDyck/` mirroring `lib/CFL`
- `Checker/`
- `Concurrency/` with subdirectories such as `MHP/`, `OpenMP/`, `MPI/`,
  `CUDA/`, `LinuxKernel/`, and `Utils/`
- `Dataflow/`
- `Fuzzing/`
- `IR/`
- `Security/`
- `Solvers/`
- `SymbolicExecution/`
- `Transform/`
- `Utils/`
- `Verification/`
- `TestUtils/` for shared test-only headers

When adding tests, mirror the source tree where practical:

- `include/Alias/...` and `lib/Alias/...` -> `tests/unit/Alias/...`
- `include/Dataflow/...` and `lib/Dataflow/...` -> `tests/unit/Dataflow/...`
- `include/Analysis/CFG/...` and `lib/Analysis/CFG/...` -> `tests/unit/Analysis/CFG/...`

## Build And Run

```bash
cmake -S . -B build
cmake --build build
cmake --build build --target lotus_unit_tests
ctest --test-dir build --output-on-failure
```

Test executables are part of the default build; `lotus_unit_tests` builds
all of them at once and `run_unit_tests` builds them before running
`ctest -L unit`.

Examples:

- build one target: `cmake --build build --target analysis_tests`
- run one case: `ctest --test-dir build -R 'analysis_tests.CFGReachabilityTest' --output-on-failure`

Every gtest case is discovered individually. Tests also carry a cost/dependency
label:

- `unit` for in-process tests with self-contained inputs
- `component` for multi-component or real-concurrency harnesses
- `integration` for process workflows or compiler-generated fixtures

Run a layer with `ctest --test-dir build -L unit`, `-L component`, or
`-L integration`. New suites default to `unit`; pass `TEST_KIND COMPONENT` or
`TEST_KIND INTEGRATION` to `add_lotus_test_suite` when appropriate.
For a mixed executable, `COMPONENT_TEST_REGEX` and `INTEGRATION_TEST_REGEX`
classify individual discovered CTest names. For example, `analysis_tests`
labels its generated TypeHierarchy and NOELLE parity fixture cases as
integration while retaining its other cases as unit tests.
Use a whole-suite `TEST_KIND` for pretty test names containing square brackets:
CMake omits those names from the discovered list used for per-case classification.

CTest case granularity does not require one executable per source file. Keep
related test sources in one suite so LLVM and Lotus libraries are linked only
once. A child directory can declare sources with
`lotus_collect_test_sources(<suite> ...)`; the parent then creates the single
binary with `add_lotus_collected_test_suite(<suite> ...)`. GTest discovery
still registers every case independently.

Each subsystem builds one merged gtest binary: `alias_tests`, `analysis_tests`,
`cfl_tests`, `checker_tests`, `concurrency_tests`, `dataflow_tests`, `ir_tests`,
`security_tests`, `solvers_tests`, `symbolic_execution_tests`, `transform_tests`, `utils_tests`, and
`verification_tests`. Fuzzing currently has one test source in
`fuzz_target_generation_test`. Loop and TypeHierarchy fixtures feed
`analysis_tests`; PrimaryComponent cases feed `cfl_tests`; parallel utility
harnesses feed `utils_tests`. The utility and dataflow harnesses also have CTest
runs with two workers. `concurrency_core_tests` and `solver_tests` are aggregate
build entry points. When adding tests, collect them into the matching subsystem
binary.

## Adding Tests

Use the small set of helpers in `tests/unit/UnitTestHelpers.cmake`:

- `add_lotus_test_suite` for normal multi-source suites
- `add_lotus_targeted_test` for a single-source test with explicit libraries
- `add_lotus_concurrency_test_suite` for concurrency suites sharing the common
  link set
- `lotus_collect_test_sources` and `add_lotus_collected_test_suite` when child
  directories contribute to one linked binary

Keep test target names stable unless you intentionally want to update external
scripts or CI filters.
