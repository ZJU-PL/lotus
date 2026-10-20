# Lotus tests

Tests follow the owning module's path in `include/` and `lib/`. Source
directories describe how a test is implemented; CTest labels describe what it
needs to run.

| Directory | Contents |
| --- | --- |
| `unit/<Module>/` | GTest sources and subsystem source manifests |
| `unit/TestUtils/` | Shared C++ test helpers |
| `regress/<Module>/` | File-based regressions, CLI registration, inputs, runners, and expected outputs |
| `integration/` | Python workflow tests that exercise installed or build-tree launchers |
| `cmake/` | Test infrastructure and its checks |
| `archive/` | Inactive historical inputs, excluded from test registration |

## Placement and registration

- Add GTest sources to the matching subsystem manifest in `unit/`. Keep related
  sources in one subsystem executable; every GTest case is discovered separately.
- Put a CLI regression's registration and oracle beside its inputs in
  `regress/<Module>/`. Register its directory through `regress/CMakeLists.txt`,
  respect `LOTUS_TEST_SUBSYSTEMS`, and guard optional executables with
  `if(TARGET ...)`. Call `lotus_label_regression_tests(<subsystem>)` after
  registration to supply labels, working directory, and a default timeout.
- Put Python workflow registration in `integration/CMakeLists.txt`. Script and
  tool build directories configure their products; tests register their consumers.
- Keep source fixtures with their owning module. Generate bitcode, reports, and
  other outputs in the build tree. Shared fixtures may be consumed across suites.
- Preserve existing CTest names when moving registration. Every active regression
  needs a registered consumer and a deterministic pass/fail oracle. Keep inputs
  without a consumer in `archive/`.

`regress/Verification/` also contains backend-specific lit suites. Their backend
configuration controls availability; their presence alone does not imply that
they are registered with CTest.

## Labels

Every registered Lotus CTest case should have `lotus`, its subsystem name, and
exactly one execution-kind label:

| Label | Meaning |
| --- | --- |
| `unit` | In-process tests with self-contained inputs |
| `component` | Component composition or a real-worker concurrency harness |
| `integration` | CLI/process workflows or tests depending on compiler-generated fixtures |

GTest suites default to `unit`. Use `TEST_KIND COMPONENT` or
`TEST_KIND INTEGRATION` for a whole suite. For mixed executables, use
`COMPONENT_TEST_REGEX` or `INTEGRATION_TEST_REGEX` to relabel matching discovered
CTest names without splitting the binary. Integration takes precedence if both
expressions match. Classification applies to execution; building a shared
executable can still require libraries and generated fixtures for all its cases.

## Build and run

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure

# List cases, select a layer, or select an owning subsystem.
ctest --test-dir build -N
ctest --test-dir build -L '^unit$' --output-on-failure
ctest --test-dir build -L '^integration$' --output-on-failure
ctest --test-dir build -L '^concurrency$' --output-on-failure
```

For a smaller configuration, select test groups and the tools their CLI cases
need:

```sh
cmake -S . -B build-apa -DLOTUS_BUILD_TESTS=ON \
  -DLOTUS_BUILD_ALL_LIBRARIES=OFF \
  -DLOTUS_TEST_SUBSYSTEMS=dataflow/apa -DLOTUS_TOOL_FAMILIES=dataflow
cmake --build build-apa -j4
ctest --test-dir build-apa --output-on-failure
```

`lotus_unit_tests` builds the selected GTest executables, including their
component and integration cases. `run_unit_tests` builds those executables and
runs the `unit` label. The other `run_*_tests` targets run already-built tests;
build the selected tools and executables first.
