# Symbolic Execution

This subtree provides Lotus's path-sensitive symbolic bug-checking stack. It executes symbolically over the guarded value-flow graph (GVFG), tracks
symbolic memory, scalar properties, and taint facts in `AnalysisState`, uses
SMT-backed path-condition reasoning to check feasibility, and runs bug checkers
through a decoupled extension layer.

## Directory Layout

Headers under `include/SymbolicExecution/` and implementations under
`lib/SymbolicExecution/` mirror one another across seven acyclic layers:

- `Support/`: module-scoped `AnalysisContext`, configurable `AnalysisLimit`
  flags, and hashing/formatting utilities (`HashUtil`).
- `Domain/`: symbolic scalar values and memory designators (`BigInteger`,
  `ProgramVar`, `PropertyValue`, `PropertyInteger`, `PropertySym`,
  `PropertyAllocator`, `SymbolicMemory`).
- `Models/`: declarative library specifications (`MemoryAPI` for allocators,
  deallocators, and known libraries; `TaintModel` for taint sources, transfers,
  and sinks).
- `Solver/`: SMT path-condition representation and solving (`ConstraintRepr`,
  `PathCondSolver`, `SummarySolverManager`).
- `Engine/`: path-sensitive execution engine (`GuardedValue`, `CStringState`,
  `TaintState`, `AnalysisExtension`, `AnalysisState`, `AnalysisSummary`,
  `AnalysisDriver`).
- `Checks/`: bug types, query hierarchy, and bug-checking engine (`BugTypes`,
  `Query`, `QueryEngine`, `MemoryChecks`, `ArithmeticChecks`).
- `Integration/`: outer pass-pipeline and reporting adapter
  (`SymbolicExecutionWrapper`).

### Layering Invariants

Dependencies flow strictly downward with zero cycles:

- `Support` has no internal dependencies.
- `Domain` depends only on `Support`.
- `Models` depends only on `Support` and `Domain`.
- `Solver` depends only on `Support` and `Domain`.
- `Engine` depends on `Support`, `Domain`, `Models`, and `Solver` — never on
  `Checks` or `Integration`. Bug checkers plug into `Engine` through the
  `StateExtension` / `SummaryExtension` hooks in `Engine/AnalysisExtension.h`.
- `Checks` depends on `Engine` and lower layers — never on `Integration`.
- `Integration` sits strictly at the outer boundary, wiring `AnalysisContext`,
  `TaintModel`, `AnalysisDriver`, `QueryEngine`, and Lotus's `BugReportMgr`.

## Execution Flow

1. `SymbolicExecutionWrapper` constructs an `AnalysisContext` for the module,
   a `TaintModel`, a `BugReportCollector`, and an `AnalysisDriver` configured
   with a `QueryEngine` factory.
2. `AnalysisDriver` computes a bottom-up call-graph order via `AnalysisContext`
   and analyzes functions in summary-friendly order.
3. For each function, `AnalysisState` transfers symbolic registers, points-to
   sets, C-string lengths, freed pointers, and taint facts over the GVFG while
   notifying `StateExtension` hooks (`onPreInstruction`, `onPostInstruction`,
   `onCallSummary`, `finalizeSummary`).
4. `QueryEngine` constructs numerical memory and arithmetic safety queries,
   inlines callee `QuerySummary` payloads at call sites, batches SMT
   satisfiability checks via `PathCondSolver`, and records confirmed reports in
   `BugReportCollector`.
5. `SymbolicExecutionWrapper` translates collected `BugReportRecord`s into
   Lotus `BugReportMgr` diagnostics.

## Headers & Starting Points

All public declarations live in `namespace SymbolicExecution` under
`include/SymbolicExecution/`. Good starting points for contributors:

- `Integration/SymbolicExecutionWrapper.h`
- `Engine/AnalysisDriver.h`, `Engine/AnalysisState.h`, `Engine/AnalysisExtension.h`
- `Checks/BugTypes.h`, `Checks/Query.h`, `Checks/QueryEngine.h`
- `Domain/ProgramVar.h`, `Domain/PropertyValue.h`, `Domain/SymbolicMemory.h`
- `Support/AnalysisContext.h` and `Models/TaintModel.h`
