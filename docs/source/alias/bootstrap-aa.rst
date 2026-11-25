BootstrapAA
===========

BootstrapAA is a standalone flow- and context-sensitive pointer alias analysis
in ``lib/Alias/InclusionBased/BootstrapAA`` implementing Vineet Kahlon's PLDI
2008 *Bootstrapping* algorithm (*"Bootstrapping: A Technique for Scalable Flow
and Context-Sensitive Pointer Alias Analysis"*, PLDI '08, pp. 249–259).

Architecture
------------

BootstrapAA executes the paper's full bootstrapping pipeline:

1. **Steensgaard partitioning & hierarchy (§2.1)**: Direct target unification
   with per-node nullability, unification-based indirect-call resolution,
   Kosaraju SCC cycle collapse, and retained ``SteensgaardHierarchy`` DAG,
   depth, and cyclic metadata. Distinct partitions are preserved even when a
   ``top`` pointer (e.g., ``main``'s ``argv``) exists.
2. **Optional one-level-flow cascade (§4)**: Optional Das (2000) directional
   top-level refinement (``Options::enable_one_level_flow``,
   ``--one-level-flow``) prior to Andersen refinement.
3. **Relevant slicing (Algorithm 1)**: Worklist-based computation of both
   relevant values/objects (``V_P``) and relevant statements (``St_P``) guided
   by the Steensgaard hierarchy.
4. **Thresholded sparse Andersen refinement (§2.1, Theorem 7)**: Sparse
   ``St_P``-restricted inclusion analysis on partitions exceeding
   ``andersen_threshold`` (default 60), building an overlapping inverse
   points-to cluster cover with deterministic adaptive threshold tuning and
   size/work guards.
5. **Guarded backward summary solver (Definition 8, Algorithms 2–5)**:
   The default backend (``SolverMode::GuardedSummary``) performs top-down
   Steensgaard-depth dovetailing (Algorithm 2, with optional literal eager
   per-depth summary precomputation via ``eager_dovetail_summaries`` /
   ``--eager-dovetail``) with monotone Pass-1 / tightening Pass-2 handling for
   cyclic depths and flow-sensitive indirect-call reachability for FSCI queries,
   backward symbolic tuple transfer restricted to ``St_P`` (Algorithm 4) over
   interned 4-atom guards (``PointsTo``, ``NotPointsTo``, ``Alias``,
   ``NotAlias``), reverse-topological call-graph SCC summary computation with
   skipping of non-modifying callees (Algorithm 5), two-phase backward ``A`` +
   forward ``Q`` summary splicing for FSCI/FSCS points-to and direct alias
   queries (Algorithm 3 & §3), and optional canonical ROBDD path sensitivity
   (``path_sensitive``, ``path_sensitive_interprocedural`` supporting both
   downward summary and upward caller splicing with cross-function LLVM
   predicate canonicalization). The forward input-state tabulation solver
   remains selectable via ``SolverMode::InputStateTabulation``
   (``--solver=tabulation``) as an independent reference oracle.
6. **Parallel per-cluster solving (§4)**: Independent clusters required by a
   query (or all clusters via ``precomputeAll()`` / ``--precompute-clusters``)
   are dispatched largest-first across worker threads with deterministic
   statistics aggregation.

Code Organization
-----------------

All headers reside under ``include/Alias/InclusionBased/BootstrapAA/``
(``Engine.h``, ``BootstrapAA.h``, and mirrored ``IR/``, ``Preprocessing/``,
``Solver/``, and ``Support/`` subdirectories), and all implementations reside
under ``lib/Alias/InclusionBased/BootstrapAA/`` (``Engine.cpp`` plus ``IR/``,
``Preprocessing/``, ``Solver/``, ``Support/``, and ``LLVM/`` subdirectories).

LLVM 14 Adapter & API
---------------------

The LLVM 14 interface is ``lotus::bootstrap::BootstrapAA`` in
``Alias/InclusionBased/BootstrapAA/BootstrapAA.h``:

- ``pointsTo(value, location, context, point)`` queries points-to sets at a
  specific program point and exact call-site context (an empty context means
  the entry activation).
- ``pointsToAllContexts(value, location, point)`` computes the context-joined
  (FSCI) points-to set.
- ``mayAlias(lhs, rhs, location, context, point)`` tests pairwise aliasing.
- ``aliases(value, location, context, point)`` and
  ``aliasesAllContexts(value, location, point)`` return cluster-scoped alias
  sets directly (Algorithm 3).
- ``fieldObjectId(allocation, byte_offset)`` resolves a flattened aggregate
  field's abstract object ID.

Following Remark 1 of the paper, the LLVM 14 adapter recursively flattens
struct types, folds arrays via ``ArrayRegion`` stride metadata, models constant
aggregate global initializers, infers heap struct layouts from unique
allocation-site bitcasts, preserves fields through ``Opcode::Field`` GEPs and
``Opcode::MemCopy`` transfers, and applies byte-range ``Opcode::Havoc``
invalidation on non-pointer stores and memory intrinsics. Resource exhaustion
always returns ``PointsToSet::top()`` with ``QueryStatus::ResourceLimit``.

Build & CLI
-----------

Build targets are ``CanaryBootstrapCore`` (dependency-free C++17),
``CanaryBootstrapAA`` (LLVM 14 adapter), and ``lotus-alias-bootstrap`` (CLI
driver supporting ``--solver=guarded|tabulation``, ``--all-contexts``,
``--print-aliases``, ``--one-level-flow``, ``--eager-dovetail``,
``--path-sensitive``, ``--path-sensitive-interproc``, ``--detailed-stats``, and
adaptive/parallel controls). Integrated GTest cases live in ``alias_tests``
(``BootstrapEngineTest.*`` and ``BootstrapAATest.*``) alongside the
``bootstrap_aa_cli_smoke`` integration test.

The model assumes sequential closed-world execution and uses conservative
unknown effects for unsupported pointer operations, opaque external calls,
exceptional call alternatives, and non-default address-space nulls. It is not
wired into a context-free LLVM AA wrapper. See
``lib/Alias/InclusionBased/BootstrapAA/README.md`` for the complete
paper-to-code mapping, API examples, limits, and validation results.
