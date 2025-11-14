Analysis Tools
==============

This page documents general-purpose analysis tools under ``tools/`` that are
not specific to a single bug class.

MCP – Model Checking Platform
-----------------------------

Model checking / verification front-end built on top of SMT solving.

**Binary**: ``mcp``  
**Location**: ``tools/mcp/mcp.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/mcp [options] file.smt2

Typical workflow:

1. Encode your verification problem in SMT-LIB2 format
2. Run ``mcp`` on the SMT2 file
3. Inspect SAT/UNSAT result and model (if any)

OWL – SMT/Model Checking Front-End
----------------------------------

``owl`` is a lightweight front-end for feeding SMT-LIB2 problems to the
configured SMT solver (Z3 in the default build).

**Binary**: ``owl``  
**Location**: ``tools/owl/owl.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/owl file.smt2

**Example**:

.. code-block:: bash

   ./build/bin/owl examples/solver/example.smt2

See :doc:`../solvers/smt_model_checking` for details about the solver stack.

PDG Query – Program Dependence Graph Queries
--------------------------------------------

Interactive and batch query engine for the Program Dependence Graph (PDG).

**Binary**: ``pdg-query``  
**Location**: ``tools/pdg-query/pdg-query.cpp``

**Usage**:

.. code-block:: bash

   # Interactive mode
   ./build/bin/pdg-query -i program.bc

   # Single query
   ./build/bin/pdg-query -q "returnsOf(\"main\")" program.bc

   # Batch queries from file
   ./build/bin/pdg-query -f queries.txt program.bc

Key features:

- Forward/backward slicing
- Information flow queries
- Security policy checks
- Subgraph export (DOT)

See :doc:`../pdg_query_language` and ``examples/pdg-queries/`` for a complete
language reference and examples.

Sprattus Analyzer – spranalyze
------------------------------

``spranalyze`` is the command-line driver for the Sprattus abstract
interpretation framework.

**Binary**: ``spranalyze``  
**Location**: ``tools/sprattus/spranalyze.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/spranalyze [options] input.bc

Sprattus focuses on configurable abstract interpretation with composable
domains. See :doc:`../analysis/numerical_abstract_interpretation` and
the Sprattus section in :doc:`../major_components` for more details on
available domains and configuration options.

