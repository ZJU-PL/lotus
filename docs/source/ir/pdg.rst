PDG — Program Dependence Graph
==============================

Overview
========

The **Program Dependence Graph (PDG)** is a fine-grained representation of
data and control dependences.
It is built directly from LLVM IR and is used for slicing, security analyses,
and other dependence-aware queries.

* **Location**: ``lib/IR/PDG/``, ``include/IR/PDG/``

Module Organization
===================

Headers and implementations use eight flat directories matching the eight CMake
targets 1:1. Public headers live under ``include/IR/PDG/`` and internal query
helpers live privately under ``lib/IR/PDG/Query/``:

* ``Core/`` (``CanaryPDGCore``): graph representation, wrappers, core utilities, debug metadata, options, and query diagnostics
* ``Construction/`` (``CanaryPDGConstruction``): structural builder, call graph, and LLVM dependence passes
* ``Query/`` (``CanaryPDGQuery``): PDG graph-query vocabulary, criteria, traversal, and concrete graph query services
* ``Models/`` (``CanaryPDGModels``): shared finding/rule types, reusable LLVM facts, and C/C++ library models
* ``Analyses/`` (``CanaryPDGAnalyses``): LLVM IR domain analyses (bounds, lifetime, state, taint, arithmetic, API, contracts, protocols)
* ``Rules/`` (``CanaryPDGRules``): rule catalog and rule engine dispatch
* ``Cypher/`` (``CanaryPDGCypher``): Cypher AST, parser, and executor
* ``Visualization/`` (``CanaryPDGVisualization``): LLVM ``GraphTraits`` and DOT printer pass

Link only the targets a consumer needs. Construction and graph queries depend on
Core; Analyses depends on Core and Models; Rules and Cypher depend on Query and
Analyses; Visualization depends on Construction. Graph queries have no
dependency on Cypher, IR analyses, or rule evaluation.

``Construction/GraphBuilder.h`` builds a structural graph into an explicitly supplied
``ProgramGraph``; ``build`` replaces prior contents and invalidates query
caches. ``bindDebugInfo`` adds debug-type annotations. Independent graph
instances own their objects and release them on destruction. The LLVM
dependence passes use the shared program graph; call graphs are owned by the
pass or caller that builds them.

Core Passes
===========

The PDG infrastructure provides several LLVM passes:

* ``DataDependencyGraph`` – builds def-use, read-after-write (RAW), and
  alias-based **data dependence** edges.
* ``ControlDependencyGraph`` – computes **control dependences** between
  basic blocks and instructions.
* ``ProgramDependencyGraph`` – combines data and control dependences and
  exposes a unified query interface.

Unified Query API
=================

The public query layer lives in ``include/IR/PDG/Query/Query.h``, a thin
umbrella header over the focused query headers. The core services are:

* ``SliceQuery`` – forward/backward slices, chops, and thin slices
* ``DependenceQuery`` – reachability and shortest-path style queries
* ``DataFlowQuery`` – reaching definitions, liveness, and control-region views
* ``TransformQuery`` – motion legality and scheduling helpers
* ``DiffQuery`` – structural differencing between query results or scopes

Additional graph services include ``SummaryQuery`` (function summaries),
``ImpactQuery`` (change ranking) and ``ResourceFlowQuery`` (resource tracking).
The separate ``Analyses/`` headers expose ``TaintAnalysis``, ``LifetimeAnalysis``,
``StateAnalysis``, ``ApiAnalysis``, ``ArithmeticAnalysis``, ``BoundsAnalysis``,
``ProtocolAnalysis`` and ``CallContractAnalysis``. ``Rules/RuleEngine.h`` orchestrates
these IR analyses and owns rule selection and findings.

Graph services share the following option/result model; semantic engines have
their own typed results:

* ``PDGQueryOptions`` – edge preset, scope, context mode, traversal limits,
  cache policy, and explanation mode
* ``PDGQueryScope`` – whole graph, explicit node set, function, or prior query
  result
* ``PDGQueryResult`` – nodes, induced edges, predecessor map, witness paths,
  distances, and diagnostics
* ``PDGCriteriaResolver`` – seeds queries from nodes, LLVM values, function
  names, callee names, source locations, and property specs. Cypher selectors
  execute in the front end and supply ``PDGCriteria::nodeSet(resultNodes)``;
  an explicitly empty selection remains distinct from omitted criteria

High-Level Usage
================

PDG is exposed as an LLVM ``ModulePass``. A typical usage pattern:

.. code-block:: cpp

   #include "IR/PDG/Construction/ProgramDependencyGraph.h"
   #include "IR/PDG/Query/SliceQuery.h"
   #include "llvm/IR/LegacyPassManager.h"
   using namespace llvm;
   using namespace pdg;

   legacy::PassManager PM;
   PM.add(new DataDependencyGraph());
   PM.add(new ControlDependencyGraph());
   auto *pdgPass = new ProgramDependencyGraph();
   PM.add(pdgPass);
   PM.run(module);

   ProgramGraph *G = pdgPass->getPDG();
   SliceQuery slicer(*G);

   PDGCriteria criteria;
   criteria.values.push_back(src);

   PDGQueryOptions options;
   options.edge_preset = PDGEdgePreset::Data;
   options.context_mode = PDGContextMode::ContextSensitive;

   auto result = slicer.forward(criteria, options, &module);

For interactive querying and slicing, see the :doc:`../user_guide/pdg_query_language`
and the ``lotus-ir-pdg-query`` tool described in :doc:`../tools/ir/index`.

The Cypher selector language implementation is separated from these concrete
analysis services under ``include/IR/PDG/Cypher/`` and
``lib/IR/PDG/Cypher/``.

The Cypher query rule set provides pattern matching over the PDG IR, including
security-focused query templates. See ``tools/ir/examples/`` for example queries
and the :doc:`../tools/ir/examples` cookbook.

`lotus-ir-pdg-query` Analysis Mode
==================================

``lotus-ir-pdg-query`` accepts raw Cypher queries and can run PDG analysis
services directly:

* ``--analysis slice-forward`` / ``slice-backward`` / ``chop``
* ``--analysis shortest-path`` / ``reaching-defs`` / ``control-region``
* ``--analysis live`` / ``dead`` / ``diff``

Analysis mode uses Cypher as a selector front end:

* ``--criteria-query`` selects analysis seeds
* ``--target-query`` selects targets for binary analyses
* ``--scope-function`` or ``--scope-query`` restrict analysis scope
* ``--edge-preset`` chooses one of ``all``, ``data``, ``control``,
  ``parameter``, ``interprocedural``, ``value-flow``, or
  ``transform-legality``
* ``--context-sensitive`` enables call/return matching
* ``--thin`` enables thin slicing semantics
* ``--format text|json|dot`` controls rendering
