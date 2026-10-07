Lotus EGraph: Equality Saturation Engine
========================================

This section documents the standalone e-graph library used for equality
saturation and rewrite-driven normalization.

Overview
--------

**Location**: ``include/Solvers/EGraph/`` (headers), ``lib/Solvers/EGraph/``
(implementation)

**Umbrella Header**: ``Solvers/EGraph.h``

**Build target**: ``LotusEGraph``

``LotusEGraph`` is a solver-agnostic C++17 e-graph library modeled after the
Rust ``egg`` library. It preserves ``egg`` semantics for the supported core
equality-saturation pipeline while exposing a Lotus-first C++ API. The library
is reusable outside SMT-specific clients and is covered by unit tests under
``tests/unit/Solvers/``.

This library is distinct from the E-graph based quantifier simplification
utilities documented in :doc:`egraphs_simp`, which live under
``lib/Solvers/SMT/QuantSimp/`` and operate on Z3 SMT formulas.

Core Components
---------------

EGraph
~~~~~~

The central ``EGraph<L, AnalysisT>`` template maintains a congruence-closed
union-find over e-classes of language nodes ``L``. It supports:

- ``add`` / ``addExpr`` — insert nodes or ``RecExpr`` terms
- ``unite`` / ``unionInstantiations`` — merge e-classes, optionally with a
  rule name for explanations
- ``rebuild`` — restore the congruence invariant after unions
- ``classes`` / ``classIds`` — enumerate e-classes
- ``equivalent`` / ``equivs`` — query term equivalence
- analysis hooks (``AnalysisT``) for data attached to each e-class
- optional JSON import/export (``toJson`` / ``fromJson`` / ``parseJson``)

UnionFind and Id
~~~~~~~~~~~~~~~~

``Id`` identifies e-nodes and e-classes; ``UnionFind`` provides the union-find
structure behind e-class merging.

RecExpr
~~~~~~~

``RecExpr<L>`` is a compact, shared-subexpression representation of a term as
a list of language nodes. It supports S-expression parsing and printing.

Language
~~~~~~~~

``SymbolLang`` is the default untyped language (operator symbol plus children).
``DynamicLang`` is a runtime-typed variant. ``LanguageOps``, ``LanguageHash``,
and ``LanguageSerializationOps`` provide the customization points used by the
e-graph.

DefineLanguage
~~~~~~~~~~~~~~

``LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE`` and
``LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE_IN`` generate variant-discriminated typed
languages from an X-macro variant list. Variants can be constants, fixed-arity
operators, variadic operators, typed payloads (``DATA``), and payload-bearing
operators (``DATA_FIXED``, ``DATA_VARIADIC``). ``TypedValueCodec<T>`` can be
specialized for payloads that do not support stream parsing.

Pattern and MultiPattern
~~~~~~~~~~~~~~~~~~~~~~~~

``Pattern<L>`` compiles an e-matching program from a pattern AST and searches
the e-graph for matches, returning substitutions. ``MultiPattern<L>`` matches
several patterns that share bound variables.

Rewrite
~~~~~~~

``Rewrite<L, A>`` pairs a searcher (pattern) with an applier (pattern or
conditional applier). Helper constructors ``makeRewrite``,
``makeConditionalRewrite``, ``makeMultiRewrite``, and borrowed rewrite builders
are provided as Lotus conveniences.

Runner
~~~~~~

``Runner<L, A>`` drives the equality-saturation loop: it repeatedly searches
for rewrite matches, applies them, and rebuilds the e-graph until saturation or
a limit is reached. Limits cover iterations, node count, and wall-clock time.
``SimpleScheduler`` and ``BackoffScheduler`` control rewrite scheduling.

Extractor
~~~~~~~~~

``Extractor<L, A, CostFn>`` picks a minimal-cost representative term from each
e-class using a cost function such as ``AstSize`` or ``AstDepth``.

Explain
~~~~~~~

With explanations enabled, the e-graph records union events and can produce
``Explanation`` objects that justify term equivalence as a sequence of rewrite
steps. ``Runner::explainEquivalence`` and ``Runner::explainMatches`` expose this
directly.

Dot Export
~~~~~~~~~~

``Dot<L, A>`` renders the e-graph as a Graphviz ``dot`` graph. It is compiled
in when ``LOTUS_EGRAPH_ENABLE_DOT`` is enabled (default ON) and is included by
the umbrella header only in that case.

Lotus Extensions
----------------

Beyond the ``egg``-aligned core, the library provides:

- ``egraphUnion`` / ``egraphIntersect`` — combine two e-graphs
- ``LanguageMapper`` / ``SimpleLanguageMapper`` — map one language to another
- JSON import/export helpers for ``EGraph``

Usage
-----

.. code-block:: cpp

   #include "Solvers/EGraph.h"

   using namespace lotus::egraph;

   // Typed languages are declared with an X-macro variant list.
   #define MATH_VARIANTS(V)                                                      \
     V(CONSTANT, Pi, "pi", _)                                                    \
     V(FIXED, Add, "+", 2)                                                       \
     V(DATA, Number, _, int64_t)

   LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE_IN(math, MathLang, MATH_VARIANTS);

   // A rewrite pairs a searcher pattern with an applier pattern.
   auto add_zero = makeRewrite<MathLang>("add-zero", "(+ ?x 0)", "?x");

   // The Runner drives the equality-saturation loop.
   Runner<MathLang> runner;
   runner.withExpr(RecExpr<MathLang>::parse("(+ 1 0)"))
         .withIterLimit(10)
         .run({add_zero});

See Also
--------

- :doc:`egraphs_simp` — E-graph based quantifier simplification for SMT
  formulas (a different component under ``lib/Solvers/SMT/QuantSimp/``)
- :doc:`libsmt` — SMT solver abstraction layer