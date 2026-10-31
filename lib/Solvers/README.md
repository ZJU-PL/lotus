# Solvers

Constraint, logic, and SMT solver integrations for Lotus.

## Generic Solver Infrastructure

- **Datalog** - Native C++ Datalog engine supporting Stratified Datalog, lattice
  fixed-point semantics, and frontends for JSON, native Datalog, and Z3 fixedpoint
- **EGraph** - Solver-agnostic equality graph and rewrite engine inspired by
  `egg`

## SMT

SMT solver wrappers and utilities:

- **LIBSMT** – Z3 API wrapper with factory pattern
- **QuantSimp** – Quantifier simplification using E-graphs
- **SMTSampler** – SMT model sampling
- **SMTStabilizer** – SMT-LIB2 normalization to stabilize solver performance under syntactic mutations
- **STAUB** – SMT Theory Arbitrage: rewrites unbounded constraints into an
  equivalent bounded theory so Z3 can solve them faster
- **SymAbs** – SMT formula abstraction (bit-vector to linear integer)
- **TUNA** – SMT↔LLVM translation and optimization, with GA-based LLVM pass selection (same name as the LotusAA strong-update tool; unrelated)

## References

- Z3: https://github.com/Z3Prover/z3
