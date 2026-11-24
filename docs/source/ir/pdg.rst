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

Headers and implementations use five flat directories; all headers, including
internal helpers, live under ``include/IR/PDG/``:

* ``Core/``: graph representation, structural builder, call graph and LLVM passes
* ``Analysis/``: semantic engines, reusable facts, library models and rule dispatch
* ``Query/``: graph-query vocabulary, criteria, traversal and concrete services
* ``QueryLanguage/``: Cypher AST, parser and executor
* ``Support/``: debug metadata, graph helpers, options, graph traits and DOT printer

Classes and CMake targets separate logical layers within these directories.
Link the targets a consumer needs: ``CanaryPDGCore``,
``CanaryPDGConstruction``, ``CanaryPDGQuery``, ``CanaryPDGSemanticSupport``,
``CanaryPDGSemantics``, ``CanaryPDGRules``, ``CanaryPDGCypher`` or
``CanaryPDGVisualization``. Construction and graph queries depend on Core;
Rules and Cypher depend on graph queries and semantic services. Graph queries
have no dependency on Cypher or rule evaluation.

``Core/GraphBuilder.h`` builds a structural graph into an explicitly supplied
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
The separate ``Analysis/`` headers expose ``TaintQuery``, ``LifetimeQuery``,
``StateQuery``, ``ApiQuery``, ``ArithmeticQuery``, ``BoundsQuery``,
``ProtocolQuery`` and ``CallContractQuery``. ``Analysis/RuleQuery.h`` orchestrates
these semantic engines and owns rule selection and findings.

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

   #include "IR/PDG/Core/ProgramDependencyGraph.h"
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
analysis services under ``include/IR/PDG/QueryLanguage/`` and
``lib/IR/PDG/QueryLanguage/``.

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
