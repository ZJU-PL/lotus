UFG — Universal Flow Graph
===========================

Overview
========

The **Universal Flow Graph (UFG)** is an object-expanded use-flow graph
baseline for the UseTraceSSA resource clients. It consumes the same
``TraceFlowGraph`` produced by the Lotus SVFG importer without requiring any
pointer analysis or library model to be rerun.

* **Location**: ``lib/IR/UFG/``, ``include/IR/UFG/``
* **Namespace**: ``lotus::ufg``
* **Core Target**: ``CanaryUFG``
* **CLI Driver**: ``lotus-ir-ufg`` (see :doc:`../tools/ir/index`)

Architecture and Construction
=============================

Lane-Based Object Expansion
---------------------------

UFG constructs an explicit lane for each object:

1. **Object Universe**: Construction enumerates the finite object universe from
   upstream guards (plus optional caller-supplied IDs).
2. **Disconnected Lanes**: For every object, it copies the temporal graph's nodes
   and eligible edges into an independent, disconnected lane.
3. **Guarded Node Effects**: Guarded node effects become ordinary events within
   that lane, preserving the certainty of the fixed-object UseTraceSSA projection.
4. **Special Sentinels**: Known-empty guards create no lane edge; unknown guards
   apply to every lane. The ``UnknownResource`` sentinel receives its own dedicated lane.

Search Engine and Defect Detection
==================================

* ``lotus::ufg::SearchEngine`` executes queries on a materialized lane. Its
  tabulation keys are ``(graph node, FSM state)``, and balanced call/return
  summaries preserve interprocedural context.
* ``lotus::ufg::DefectDetector`` exposes ``run`` and ``scan`` interfaces matching
  the UseTraceSSA detector for checking:
  - **Double Free** (``double-free``)
  - **Use After Free** (``use-after-free``)
  - **Memory Leak** (``memory-leak``)
  - **File Leak** (``file-leak``)

Context Sensitivity and Budgets
===============================

* **Bounded Context**: Default bounded call-string depth is 3. Configurable via
  ``--context-limit=k`` (or ``--context-depth=k``).
* **Unbounded Context**: Use ``--context-limit=unlimited`` (alias ``--unbounded-context``)
  for unbounded Dyck summaries.
* **Context Insensitive**: ``--context=insensitive`` disables context matching.
* **Search Budgets**: Optional ``--max-product-states`` and ``--max-summary-pairs``
  bound exploration per lane query.
