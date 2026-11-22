# Conditional Dyck reachability

C++17 port of ESOP 2017 ConCRA, with ConCRA-f and ConCRA-k library
summarization and reusable client analysis. Link `CanaryConditionalDyck` and
include `CFL/ConditionalDyck/ConditionalDyck.h`.

- Public API: `lotus::cfl::conditional_dyck::{Graph, Library, Solver, Summary}`.
- Default: ConCRA-1. `Options{std::nullopt}` selects full premise sets.
- `Solver::solve` computes ordinary whole-graph reachability.
- `Solver::analyze` returns client-to-client reachability from an immutable
  summary; it validates the paper's boundary and callback contract.
- All storage is owned per operation or by a summary. IDs are full-width values,
  and repeated or concurrent clients do not share mutable analysis state.

See `docs/source/cfl/conditional_dyck.rst` for the example, supported languages,
input contract, implementation mapping and validation commands.
