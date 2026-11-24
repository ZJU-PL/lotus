# Abstract Execution (AE)

AE is Lotus's abstract-interpretation engine for memory-safety checking. Its
core is migrated from SVF's `AbstractExecution`; use-after-free, invalid-free,
and memory-leak checks are Lotus extensions.

The design follows *Precise Sparse Abstract Execution via Cross-Domain
Interaction* (Cheng, Wang, and Sui, ICSE 2024).

## Checks

| Check | Detector | Default |
|---|---|---|
| Buffer overflow | `BufOverflowDetector` | enabled |
| Null dereference | `NullptrDerefDetector` | enabled |
| Use after free | `UseAfterFreeDetector` | enabled |
| Invalid free | `InvalidFreeDetector` | enabled |
| Memory leak | `MemLeakDetector` | disabled |

## Architecture

```text
AbstractInterpretation
├── AbstractStateStore       per-instruction states and sparse layouts
├── AbstractState            SSA values, memory objects, object sizes, frees
├── AbstractValue
│   ├── IntervalValue        numeric intervals
│   └── AddressValue         points-to sets
├── ICFGWTO                  loop/recursion fixpoint iteration
├── RelationSolver           Z3 solver utilities (outside normal execution)
├── SVFIRWrapper             AserPTA interface
└── AEDetector               bug detectors and reporting
```

The engine analyzes instructions in WTO order, widens cycles, narrows where
enabled, refines branch facts, and runs detectors at each interpreted
instruction. Recursive SCCs are iterated to a summary fixpoint.

## Quick Start

```bash
lotus-check --engine=ae --checks=buffer-overflow,null-deref program.bc
```

Useful options:

```text
--ae.state-store=dense|semi-sparse|sparse   state retention (default: dense)
--ae.sparsity=...                           alias for --ae.state-store
--ae.recursion-mode=top|widen-only|widen-narrow
--ae.widen-delay-iterations=3
--ae.fun-entry=main|no-main
--ae.timeout=0                              whole-run seconds; 0 disables
--ae.checkpoint-policy=permissive|strict
--ae.state-store-stats
```

`--ae.fun-entry=main` analyzes the `main`-rooted call graph (`svf.main` is also
accepted). The CLI rejects modules without a defined entry in this mode.
`no-main` starts from one representative of every call-graph root SCC, using
resolved indirect-call edges as well as direct calls.
On timeout, findings collected so far are still reported.

## State Layouts

`dense` keeps a full abstract state at every instruction. It is the safest
choice for clients that inspect arbitrary instruction states.

`semi-sparse` stores SSA definitions once in a run-wide table while keeping
memory-object maps per instruction. `sparse` additionally uses the SVFG to
collect reaching object definitions for loads.

Sparse layouts drop unobservable transient states. Block heads, terminators,
call sites, allocas, and other query points are pinned. Detector execution is
suppressed only during widening, where deliberately temporary approximations
would create spurious findings.

Branch-refined SSA snapshots are path-local. They are not scattered into the
run-wide definition table, because that would leak one edge's facts into a
sibling edge. Formal parameters are likewise activation-local so recursive
calls do not overwrite an outer activation's arguments.

## Analysis Semantics

- Stores use SVF's strong-update rule only for a single concrete, non-summary,
  non-recursive-local object. Other may-alias stores are weak joins.
- GEP objects are field-sensitive. Their `(base object, byte offset)` identity
  and offset-from-base are run-wide, matching SVF's global SVFIR objects and
  preventing branch-local ID collisions.
- External allocation, reallocation, free, memory, string, formatted-output, and
  libc APIs are modeled without a separate `extapi.bc` input. Unknown allocation
  sizes use a conservative field limit.
- `AbstractInterpretation` is a process-wide singleton, but `runOnModule`
  recreates the pointer-analysis pass and clears all module-owned registries.

## Testing

The main suites are:

- `checker_tests`: AE semantics, detectors, interprocedural behavior, sparse
  equivalence, and instruction parity.

`tests/unit/Checker/AE/AEInstructionParityTest.cpp` covers LLVM cases that
previously diverged from SVF or between storage layouts, including floating
binary operators, `addrspacecast`, `freeze`, constant selects/aggregates and
vectors, signed/unsigned casts and comparisons, switch-default pruning,
constant call arguments, pointer offsets joined through PHIs, and struct-member
subobject bounds. Additional regressions cover vector-GEP lane offsets, bounds
from masks and unsigned remainders, casted branch reloads, bounded-memory libc
searches, and the no-definition module case.

The [migration audit](MIGRATION_AUDIT.md) records a 223-input differential run
against the local LLVM 14 SVF build, plus reduced AE-specific bug examples.
The reusable runner is `scripts/verification/ae_differential.py`. Re-run both
engines after changing transfer functions, branch refinement, detector precision,
or external-API models. Output differences alone do not establish a Lotus bug.

## Limitations

- Interval arithmetic is not fully relational or bit-precise.
- Large programs with deep recursion or many may-alias targets can be slow.
- Complex ownership transfer can limit memory-leak precision.
- Unmodeled external functions return conservative unknown values.
- Findings require manual review.
- Constant ctype calls use the analyzer host's libc and locale; cross-target
  libc behavior and program-driven locale changes are not modeled.

## References

1. X. Cheng, J. Wang, Y. Sui. *Precise Sparse Abstract Execution via
   Cross-Domain Interaction.* ICSE 2024.
2. P. Cousot, R. Cousot. *Abstract Interpretation.* POPL 1977.
3. F. Bourdoncle. *Efficient Chaotic Iteration Strategies with Widenings.* 1993.
