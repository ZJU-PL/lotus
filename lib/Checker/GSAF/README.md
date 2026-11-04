# GSAF layout

Public headers mirror this directory under `include/Checker/GSAF`.

| Directory | Responsibility |
| --- | --- |
| `API` | Vulnerability and trace definitions, registration, composition, and the query view over Lotus annotation models. `DefaultModels.h.in` embeds the profiles from `config/gsaf`. |
| `Engine` | Module/function analysis, taint traversal, SMT encoding, summaries, analysis traces, composite trace matching and graph statistics. |
| `Checkers` | Built-in vulnerability policies: use after free and taint. |
| `Report` | Trace diagnostic decorators for the Lotus bug report framework. |
| `Support` | Engine options, native GVFG query helpers, checker analysis access, mask containers and graph-object ordering. |

`API/Trace` contains the analysis trace and its scope-aware builder. It is
shared by the engine and reporting. `Support` provides `GraphQueries` (query helpers and object ordering),
`CheckerServices` (analysis pass initialization and instruction helpers), `Options`, and `MaskMap`. The engine contains the module driver (`Checker`), the solver wrapper (`Solver`),
summary representations (`Summaries`), and function analysis split across intra-procedural traversal (`IntraAnalysis.cpp`)
and inter-procedural summary inlining (`InterAnalysis.cpp`), defined by `FunctionAnalyzer.h`. Small graph-statistics and composite-orchestration
helpers share the module-pass implementation; symbolic and trace summaries
share one summary header.

Reusable facilities belong to their Lotus subsystems: library facts in
`Annotation`, heap analysis in `Analysis/Memory`, debug expression rendering in `Analysis/DebugInfo`,
diagnostic templates/composition in `Checker/Framework/DiagnosticEvent` and
`CheckerDiagnostic`, and graph/solver APIs
in `IR/GVFG`. Checker trace containers and reporting metadata live in
`Checker/Framework/ValueTrace.h`; `Checker/Framework/LLVMValueTrace.h` provides
the LLVM value-pair representation. Graph-specific traces and conversion stay
in GVFG. LLVM/GVFG narrative rendering stays in
`GSAF/Report`; final steps and metadata use the shared `BugReport` implementation.
GSAF does not maintain a second graph representation.

Build and run:

```sh
cmake --build build --target lotus-check checker_tests -j8
build/bin/tests/checker_tests --gtest_filter='GSAF*'
build/bin/lotus-check --engine=gsaf --checks=taint,use-after-free input.bc
```

Use `--checks=all` for the two current built-in checks. Additional checker
implementations should register explicitly through the API and be included
in `initializeBuiltinVulnerabilities()`, so static-library linking retains
those registrations. Update the unified frontend's check descriptors when
adding a built-in check.
