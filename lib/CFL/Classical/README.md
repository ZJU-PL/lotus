# Classical CFL Reachability

The implementation mirrors `include/CFL/Classical`:

- `Core/` contains solver-independent grammars, graphs, relations, validation,
  and recursive-state-machine support.
- `Solvers/Engines/` contains solving algorithms: the classical transitive
  closure, PEARL, POCR/FOCR, Skewed Tabulation, Sqid, Stg, and the specialized
  [SubcubicAA PEG alias analysis](Solvers/Engines/SubcubicAA/README.md).
- `Solvers/Preprocessing/` contains graph simplification and RSM foldability.
- `Clients/Alias/` and `Clients/ValueFlow/` are the two analysis clients.

Algorithms belong under `Solvers/Engines`; POCR, PEARL, Sqid, and Stg are
engines, not clients.

`lotus-cfl-subcubic-aa` exposes the OOPSLA 2014 memory/value alias algorithms
on `a`/`d` pointer expression graphs. It has a specialized API rather than an
arbitrary-grammar `SolverSession` backend.
