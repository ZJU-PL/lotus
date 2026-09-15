BootstrapAA
===========

BootstrapAA is an independent flow- and context-sensitive points-to analysis in
``lib/Alias/InclusionBased/BootstrapAA``. It uses Steensgaard partitioning,
retained hierarchy/depth metadata, hierarchy-aware dependency slicing,
thresholded Andersen refinement, and per-cluster input-state summary tabulation.
Its design is based on Kahlon's PLDI 2008 *Bootstrapping* paper, but it does not
yet reproduce the paper's guarded update-sequence summaries or FSCI
hierarchy-dovetailing algorithm.

The LLVM 14 API is ``lotus::bootstrap::BootstrapAA`` in
``Alias/InclusionBased/BootstrapAA/BootstrapAA.h``. Queries take an LLVM pointer
value, a program-point instruction, and a complete call-site context. An empty
context identifies the program-entry activation. ``pointsToAllContexts`` is an
explicitly context-joined query. Resource exhaustion returns top with an explicit
status rather than a partial points-to set.

Points-to sets use a dependency-free sparse word-bitset. Statistics include
partition and cluster distributions, hierarchy depth, cover overlap, context
cache behavior, preprocessing time, and per-cluster solve time. The LLVM
adapter recognizes common allocation and read-only interior-pointer functions,
models ``realloc`` alternatives, resolves direct calls through aliases, and
preserves zero-length memory intrinsics.

Large partitions use a deterministic adaptive Andersen threshold based on the
observed reduction in maximum cluster size. Refinements achieving less than a
25% reduction are rejected in favor of the original Steensgaard partition.
Adaptive partition-size and partition-by-hierarchy work guards prevent
unbounded Andersen preprocessing on large coarse partitions.
Function summaries are prioritized by a conservative call-graph SCC
condensation, and independent clusters needed
by a query can be constructed in parallel. Fixed-threshold and serial modes
remain available for reproducibility. ``precomputeAll()`` and the
``--precompute-clusters`` CLI option enable parallel eager evaluation of
disjoint clusters; ``--all-contexts`` uses eager evaluation automatically.

Build targets are ``CanaryBootstrapCore``, ``CanaryBootstrapAA``, and
``lotus-alias-bootstrap``. The integrated GTest target is
``bootstrap_aa_tests``.

The adapter is field-insensitive, models sequential closed-world execution, and
uses conservative unknown effects for unsupported pointer operations, external
calls, aggregate byte layouts, and exceptional call alternatives. It is not wired
into a context-free LLVM AA wrapper. The implementation README contains the full
paper-to-code mapping, query semantics, limitations, build commands, and actual
validation status:
``lib/Alias/InclusionBased/BootstrapAA/README.md``.
