Demand-Driven APA
=================

The ``DemandAPA`` engine provides a demand-driven formulation of Affine Program Analysis (APA), implementing the algorithm from OOPSLA 2023.

**Location**: ``include/Dataflow/DemandAPA/``, ``lib/Dataflow/DemandAPA/``

**CLI Tool**: ``tools/dataflow/DemandAPA/`` (binary ``lotus-dfa-demand-apa``)

Overview
--------

While the standard APA engine operates in a comprehensive, forward-summary or state-elimination mode, the **DemandAPA** engine is optimized for answering targeted data-flow queries on-demand. 

This engine is particularly effective when analyzing large Boolean programs or resolving localized properties, as it avoids computing the full algebraic summary for entire procedures when only a fraction is necessary to answer the query.

Key Features
------------

- **Demand-Driven Evaluation**: Only analyzes paths and summaries necessary to satisfy the requested queries.
- **Tree Decompositions**: Leverages modern tree-decomposition heuristics (via `flow-cutter-pace17/20`) to find optimal variable elimination orderings.
- **BDD Integration**: Uses the ``buddy`` Binary Decision Diagram library for compact algebraic state representation and fast subset/equality comparisons.
- **Boolean Program Support**: Natively parses and evaluates CEGAR-generated Boolean programs using the updated Boolean Program Frontend.

Dependencies
------------

DemandAPA introduces two major vendored dependencies in ``third-party/``:
* ``buddy-2.4``: A widely used BDD package for boolean function manipulation.
* ``flow-cutter-pace17`` / ``flow-cutter-pace20``: Tree decomposition libraries for optimizing query evaluation paths.

Usage
-----

DemandAPA is primarily driven via its CLI tool:

.. code-block:: bash

   ./build/bin/lotus-dfa-demand-apa --query=target_node input.bp

The tool accepts standard Boolean Program (``.bp``) files, which can be pre-processed or normalized using the ``BooleanProgramNormalizer`` under ``tools/verifier/boolean-program/``.
