Major Components Overview
==========================

This page consolidates the high-level component inventory that used to live
in ``README.md``. Each section links to the dedicated documentation page where
you can find deeper usage guides and configuration details.

Alias Analysis
--------------

See :doc:`../alias/alias_analysis` for detailed instructions and command examples.

* **AllocAA** – Lightweight alias analysis built from simple heuristics for
  allocation tracking.
* **DyckAA** – Unification-based exhaustive alias analysis
  (``lib/Alias/DyckAA``).
* **CFLAA** – Context-Free Language alias analysis taken from LLVM 14.0.6 and
  preserved in Lotus for LLVM 15+ users.
* **Sea-DSA** – Context-sensitive and field-sensitive analysis with
  Sea-DSA (``lib/Alias/seadsa``) requiring Boost.
* **Andersen** – Context-insensitive points-to analysis without on-the-fly
  call-graph construction (``lib/Alias/Andersen``).
* **FPA** – Function Pointer Analysis toolbox (FLTA, MLTA, MLTADF, KELP) under
  ``lib/Alias/FPA`` for resolving indirect calls.
* **DynAA** – Dynamic checker living in ``tools/dynaa`` that validates static
  alias analyses against runtime traces.
* **OriginAA** – K-callsite-sensitive, origin-sensitive analysis targeting
  thread-creation semantics.

Intermediate Representations
----------------------------

See :doc:`../ir/index` for builder APIs and code snippets.

* **Program Dependence Graph (PDG)** – Captures fine-grained data/control
  dependencies.
* **Static Single Information (SSI)** – Planned extension of SSA to encode
  predicate information.
* **DyckVFG** – Value Flow Graph variant designed for Dyck-based alias
  analyses (``lib/Alias/DyckAA/DyckVFG.cpp``).

Constraint Solving
------------------

See :doc:`../solvers/index` for solver APIs.

* **SMT Solving** – Z3-backed reasoning under ``lib/Solvers/SMT``.
* **Binary Decision Diagram (BDD)** – CUDD-based symbolic set operations
  (``lib/Solvers/CUDD``).
* **Weighted Pushdown Systems (WPDS)** – Interprocedural reachability library
  (``lib/Solvers/WPDS``).

Abstract Interpretation
-----------------------

See :doc:`../tools/verifier/clam/index` for CLAM and :doc:`../tools/verifier/sprattus/index`
for higher-level abstractions.

* **CLAM** – Modular AI-driven static analyzer with multiple abstract domains
  (``tools/clam`` and ``lib/Apps/clam``).
* **Sprattus** – Configurable abstract interpretation framework with domain
  composition (``lib/Analysis/Sprattus`` and ``include/Analysis/Sprattus``).

Utilities and Reachability
--------------------------

See :doc:`../utils/index` and :doc:`../cfl/index` for extended guides.

* **cJSON** – Lightweight JSON parser (``include/Support/cJSON.h``).
* **Transform** – LLVM bitcode transformation passes housed in ``lib/Transform``.
* **CFL Reachability** – General-purpose CFL reachability utilities and
  tooling (``tools/cfl``) including Graspan and CSR-based engines.

