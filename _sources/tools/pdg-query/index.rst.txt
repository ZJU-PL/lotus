PDG Query – Program Dependence Graph Queries
==============================================

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

See :doc:`../../user_guide/pdg_query_language` and ``examples/pdg-queries/`` for a complete
language reference and examples.

