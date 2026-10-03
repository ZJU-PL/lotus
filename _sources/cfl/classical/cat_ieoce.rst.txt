CAT and IEOCE Engines
=====================

Lotus contains faithful, SVF-independent implementations of two more recent
classical CFL-reachability algorithms:

* CAT (context-aware tabulation) prunes redundant derivation work using
  per-symbol usage contexts and guarded grammar rewrites.
* IEOCE (iterative-epoch online cycle elimination) collapses cycles during
  solving and, in its OCR variant, maintains a minimum-equivalent graph.

Both engines share the input vocabulary and the compressed result
representation of the ``Skewed`` engine through
``Solvers/Engines/Common/Reachability.h``. No import or conversion is
necessary between the three solvers.

CAT: Context-Aware Tabulation
-----------------------------

Paper
~~~~~

The engine implements Shi and Li, ICSE 2026
(`DOI 10.1145/3744916.3773222 <https://doi.org/10.1145/3744916.3773222>`__).
The header describes it as an independent implementation of Figure 3 and
Algorithm 2 of the paper.

Problem and key idea
~~~~~~~~~~~~~~~~~~~~

The engine is a context-aware tabulation. Before solving, it analyzes the
grammar and annotates the graph with usage contexts:

``analyzeGrammar``
   Computes the ``first``, ``last``, ``left``, and ``right`` terminal sets of
   every symbol and records which symbols have unary uses. Two semantics are
   available: the literal Figure-3 inference rules, including their
   conservative epsilon propagation, and Definitions 4.1/4.2, under which a
   binary right-hand side is nullable iff both operands are nullable.

``annotateGraph``
   Computes per-node usage contexts. For each symbol, a node either has
   incoming or outgoing edges in the base graph, or the context holds
   everywhere and is recorded only as a universal flag. Sparse sets store the
   remaining contexts.

``rewriteTransitivity``
   Applies the Section 4.2.1 language-preserving transitivity rewrites. The
   guards never add epsilon to a nonnullable transitive symbol, reverse
   prefix-closure orientation only for an epsilon-based one-sided closure
   (general ``B X -> X B`` replacement is not language preserving), and
   otherwise expand left and right consumers in their original order. The
   transformation records every rewrite event and the symbols skipped by the
   guards.

``solve``
   Runs the tabulation with the configured ``Options``. ``Options`` selects
   the ``FirstLastSemantics``, whether transitivity rewriting runs, whether
   the earlier explicitly limited static ``Skewed`` pass is reused
   (``conservative_static_skewing``), and whether propagating symbols and
   dynamic skewing are enabled. The ``Stats`` record the worklist behavior,
   the attempts fully pruned by the usage contexts, how insertions were
   indexed (incoming-only, outgoing-only, fully indexed, or unindexed), and
   how many context annotations and universal contexts were produced.

Lotus implementation
~~~~~~~~~~~~~~~~~~~~

Public API
   ``include/CFL/Classical/Solvers/Engines/CAT/ContextAwareTabulation.h``

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/CAT/ContextAwareTabulation.cpp``

Integration
   ``SolverBackend::Cat`` in ``SolverSession`` and ``--solver cat`` in the
   classical, alias, and value-flow drivers.

The engine shares the ``common`` input types (``Grammar``, ``Graph``,
``Query``, ``Limits``) and the compressed ``Reachability`` result with the
``Skewed`` and IEOCE engines. ``Result`` additionally exposes the working
grammar, the grammar analysis, the applied transformation, and the optional
static-skewing rewrites. ``RebuildingSession`` wraps ``solve`` for incremental
terminal-edge additions; base-graph changes invalidate the offline context
annotations and rebuild rather than reuse a stale context.

Validation
~~~~~~~~~~

The Classical CFL tests compare the complete labeled relation of ``Cat`` with
the ``SparseSet`` baseline and exercise incremental additions. A focused test
covers usage-context pruning of adjacency.

IEOCE: Iterative-Epoch Online Cycle Elimination
-----------------------------------------------

Paper
~~~~~

The engine implements Xu et al., OOPSLA 2024
(`DOI 10.1145/3649862 <https://doi.org/10.1145/3649862>`__), covering
Sections 4-5 and Algorithms 2-5 of the paper.

Problem and key idea
~~~~~~~~~~~~~~~~~~~~

The engine eliminates cycles online while solving. ``checkTransitiveSymbol``
and ``findTransitiveSymbols`` identify the transitive symbols of a grammar
according to Definitions 4.1-4.6, checked against all requested nonterminals
rather than only the start symbol. ``TransitivityCheck`` reports whether a
symbol is eligible, whether it is doubly recursive, and its nonabsorbing
targets and intransitive combinations.

``solve`` then runs iterative epochs. Each epoch collapses strongly connected
components into quotient nodes, replays the quotient facts, and continues
until no further collapse is possible. The ``IeaOcr`` variant additionally
maintains a minimum-equivalent graph (MEG) and removes redundant edges from
it. ``Stats`` record the quotient nodes, epochs, SCC passes, collapsed
components, merged nodes, quotient replays, MEG insertions and removals, and
whether the run fell back to ordinary tabulation.

``Options`` selects the ``Direction`` (directed or bidirected) and the
``Variant`` (``Iea`` or ``IeaOcr``). Bidirected mode requires a total
involution and a syntactically reverse-closed grammar; ``solve`` additionally
verifies reverse closure of the terminal input edges. An explicit
``transitive_symbol`` may be requested; an empty selection deterministically
chooses the first admissible symbol, preferring an ``A A`` rule for
``IeaOcr``. Requesting an unsafe symbol is an error. Generic grammars may have
no collapsible relation; unless ``require_optimization`` is set, ``solve``
then performs exact ordinary tabulation and reports the fallback.
``trace_epochs`` records per-epoch statistics, and ``check_invariants`` runs
expensive diagnostic checks on quotient canonicalization and, for ``IeaOcr``,
on MEG reachability and post-collapse minimality.

Lotus implementation
~~~~~~~~~~~~~~~~~~~~

Public API
   ``include/CFL/Classical/Solvers/Engines/IEOCE/IterativeEpoch.h``

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/IEOCE/IterativeEpoch.cpp``

Integration
   ``SolverBackend::Iea`` and ``SolverBackend::IeaOcr`` in ``SolverSession``
   and ``--solver iea`` / ``--solver iea-ocr`` in the classical, alias, and
   value-flow drivers.

The engine shares the ``common`` input types and the compressed
``Reachability`` result with the ``Skewed`` and CAT engines. ``Result``
additionally exposes the selected transitive symbol, the admissible symbols,
and the per-epoch trace. ``RebuildingSession`` wraps ``solve`` for incremental
terminal-edge additions; base-graph changes invalidate the quotient graph and
rebuild rather than reuse a stale partition.

Validation
~~~~~~~~~~

The Classical CFL tests compare the complete labeled relation of ``Iea`` and
``IeaOcr`` with the ``SparseSet`` baseline and exercise incremental additions.
Focused tests cover cycle contraction for both variants, the ``IeaOcr`` MEG
insertions, and the compressed storage of contracted results.

See also
--------

:doc:`/cfl/classical/classical` documents the shared ``SolverSession`` backends, the common
relation interface, and the command-line drivers.