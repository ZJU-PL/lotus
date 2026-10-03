Demand-Driven APA
=================

The ``DemandAPA`` engine provides a demand-driven formulation of Algebraic Program Analysis (APA), implementing the algorithm from OOPSLA 2023.

**Location**: ``include/Dataflow/DemandAPA/``, ``lib/Dataflow/DemandAPA/``

**CLI Tool**: ``tools/dataflow/DemandAPA/`` (binary ``lotus-demand-apa``)

Overview
--------

While the standard APA engine precomputes whole-procedure summaries (forward-summary or state-elimination mode), the **DemandAPA** engine answers targeted data-flow queries on demand.

This engine is particularly effective when analyzing large Boolean programs or resolving localized properties, as it avoids computing the full algebraic summary for entire procedures when only a fraction is necessary to answer the query.

Key Features
------------

- **Demand-Driven Evaluation**: Only analyzes paths and summaries necessary to satisfy the requested queries.
- **Tree Decompositions**: Uses the tree-decomposition heuristics from `flow-cutter-pace17/20` to pick variable elimination orders.
- **BDD Integration**: Uses the ``buddy`` Binary Decision Diagram library for compact algebraic state representation and fast subset/equality comparisons.
- **Boolean Program Support**: Parses and evaluates CEGAR-generated Boolean programs through the Boolean Program Frontend.

Dependencies
------------

DemandAPA introduces two major vendored dependencies in ``third-party/``:
* ``buddy-2.4``: A widely used BDD package for boolean function manipulation.
* ``flow-cutter-pace17`` / ``flow-cutter-pace20``: Tree decomposition libraries for optimizing query evaluation paths.

Usage
-----

DemandAPA is driven by a batch CLI that walks a dataset directory, records
per-program timings into ``<dataset-root>/results.csv``, and takes a timeout
and a query-count limit per program:

.. code-block:: bash

   ./build/bin/lotus-demand-apa TIMEOUT COUNT RESET dataset-root

``TIMEOUT`` is the per-program budget in seconds, ``COUNT`` the number of
queries per program, ``RESET`` re-creates ``results.csv`` when non-zero, and
``dataset-root`` must end in ``/``. The driver exits with status 2 if any
argument is missing.

Input programs are normalized Boolean Program (``.bp``) files, which can be
pre-processed or normalized using the ``BooleanProgramNormalizer`` under ``tools/verifier/boolean-program/``.
