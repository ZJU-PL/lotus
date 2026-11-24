# Summary and Side-Effect Analyses

Function- and allocation-level summary and side-effect analyses built on LLVM IR.

| Subdir | Target | Purpose |
|--------|--------|---------|
| **Purity** | `CanaryPurityAnalysis` | Function purity classification (`Const`, `Pure`, `Impure`, `Unknown`), external summary storage, attribute inference, and unknown-impact modeling. |
| **ParameterSummary** | `CanaryParameterSummary` | Interprocedural parameter effect summaries (freed, dereferenced, allocated return) and resource-table support. |
| **Multiplicity** | `CanaryMultiplicity` | Allocation multiplicity classification (`Unique` vs `Summary`) for globals, stack allocations, and heap allocations. |
