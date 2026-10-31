PDG Query Language (Cypher)
============================

The Program Dependence Graph (PDG) Query Language uses Cypher, a declarative graph query language, to query dependencies, perform slicing, and verify security policies on program dependence graphs.

Overview
--------

Cypher provides a declarative way to:

- Query program dependencies (data and control)
- Perform forward and backward slicing
- Check information flow properties
- Verify security policies
- Explore shortest paths with the built-in ``shortest-path`` analysis mode
- Analyze parameter dependencies

The language supports:

- Pattern matching (nodes and relationships)
- WHERE clauses for filtering
- Variable-length pattern matching
- Set operations (UNION, intersection via shared variables)
- Aggregations (``COUNT``) and ordering (``ORDER BY``)

Getting Started
---------------

Building the PDG
~~~~~~~~~~~~~~~~

First, compile your program and build the PDG:

.. code-block:: bash

   # Compile to LLVM IR
   clang -emit-llvm -g -c program.c -o program.bc
   
   # Run PDG query tool
   ./build/bin/lotus-ir-pdg-query program.bc

Interactive Mode
~~~~~~~~~~~~~~~~

Use ``-i`` flag for interactive queries:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -i program.bc
   
   PDG Query (type 'help' or 'quit')
   > MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n
   [Results displayed]
   > MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET)-[*]->(n) WHERE input.name = 'getInput' RETURN n
   [Slice displayed]
   > exit

Each interactive line is parsed as a complete query. Use ``-q`` for a single
multi-line query or ``-f`` for a batch file.

Single Query Mode
~~~~~~~~~~~~~~~~~

Use ``-q`` flag for single query:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -q "MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n" program.bc

Batch Query Mode
~~~~~~~~~~~~~~~~

Use ``-f`` flag to run queries from file:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -f queries.txt program.bc

Language Reference
------------------

Basic Node Queries
~~~~~~~~~~~~~~~~~~

Get All Nodes
^^^^^^^^^^^^^

Returns all nodes in the PDG.

**Syntax**: ``MATCH (n) RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n) RETURN n

**Returns**: All nodes in the PDG.

Query Nodes by Type
^^^^^^^^^^^^^^^^^^^

Select nodes by their label (type).

**Syntax**: ``MATCH (n:LABEL) RETURN n``

**Node Types (Labels)**:

- ``INST_FUNCALL`` - Function call instructions
- ``INST_RET`` - Return instructions
- ``INST_BR`` - Branch instructions
- ``INST_OTHER`` - All other instructions (loads, stores, allocas, GEPs, ...)
- ``FUNC_ENTRY`` - Function entry points
- ``PARAM_FORMALIN`` - Formal input parameters
- ``PARAM_FORMALOUT`` - Formal output parameters (return values)
- ``PARAM_ACTUALIN`` - Actual input parameters
- ``PARAM_ACTUALOUT`` - Actual output parameters
- ``VAR_STATICALLOCGLOBALSCOPE`` - Statically allocated globals
- ``VAR_STATICALLOCMODULESCOPE`` - Module-scoped statically allocated variables
- ``VAR_STATICALLOCFUNCTIONSCOPE`` - Function-scoped statically allocated variables
- ``VAR_OTHER`` - Other variables
- ``FUNC`` - Function nodes
- ``CLASS`` - Class nodes
- ``ANNO_VAR`` - Variable annotations
- ``ANNO_GLOBAL`` - Global annotations
- ``ANNO_OTHER`` - Other annotations

The query engine also accepts the node groups ``INST``, ``VAR``, ``PARAM``,
and ``ANNO`` to match all labels in that group.

**Example**:

.. code-block:: cypher

   MATCH (n:INST_FUNCALL) RETURN n
   MATCH (n:INST_BR) RETURN n

Function-Specific Queries
~~~~~~~~~~~~~~~~~~~~~~~~~~

Return Values of a Function
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Returns the return values of a function.

**Syntax**: ``MATCH (n:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET) WHERE n.name = 'functionName' RETURN ret``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET)
   WHERE n.name = 'main'
   RETURN ret

   MATCH (n:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET)
   WHERE n.name = 'getInput'
   RETURN ret

**Returns**: Nodes representing return values of the specified function.

Formal Parameters of a Function
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Returns the formal parameters of a function.

**Syntax**: ``MATCH (n:FUNC_ENTRY)-[:PARAMETER_IN]->(param:PARAM_FORMALIN) WHERE n.name = 'functionName' RETURN param``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)-[:PARAMETER_IN]->(param:PARAM_FORMALIN)
   WHERE n.name = 'process_data'
   RETURN param

**Returns**: Nodes representing formal input parameters.

Function Entry Points
^^^^^^^^^^^^^^^^^^^^^^

Returns the entry points of a function.

**Syntax**: ``MATCH (n:FUNC_ENTRY) WHERE n.name = 'functionName' RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)
   WHERE n.name = 'main'
   RETURN n

**Returns**: Function entry nodes.

Edge Queries
~~~~~~~~~~~~

Query Edges by Type
^^^^^^^^^^^^^^^^^^^

Select edges by their relationship type.

**Syntax**: ``MATCH ()-[r:TYPE]->() RETURN r``

**Edge Types (Relationship Types)**:

- ``IND_CALL`` - Indirect call edges
- ``CONTROLDEP_CALLINV`` - Control dependency from a call site to the callee
- ``CONTROLDEP_CALLRET`` - Control dependency from the callee return to the call site
- ``CONTROLDEP_ENTRY`` - Control dependency from the function entry
- ``CONTROLDEP_BR`` - Control dependency from branches
- ``CONTROLDEP_IND_BR`` - Control dependency from indirect branches
- ``DATA_DEF_USE`` - Data definition-use edges
- ``DATA_RAW`` - Read-after-write edges
- ``DATA_READ`` - Read edges
- ``DATA_ALIAS`` - Alias edges
- ``DATA_RET`` - Return-value data edges
- ``PARAMETER_IN`` - Parameter input edges
- ``PARAMETER_OUT`` - Parameter output edges
- ``PARAMETER_FIELD`` - Field parameter edges
- ``GLOBAL_DEP`` - Global dependency edges
- ``VAL_DEP`` - Value dependency edges
- ``CLS_MTH`` - Class-to-method edges
- ``ANNO_VAR`` - Variable annotation edges
- ``ANNO_GLOBAL`` - Global annotation edges
- ``ANNO_OTHER`` - Other annotation edges
- ``TYPE_OTHEREDGE`` - Type-related edges

The aliases ``CALL_INV``, ``CALL_RET``, ``PARAM_IN``, and ``PARAM_OUT`` are
also accepted in relationship patterns.

**Example**:

.. code-block:: cypher

   MATCH ()-[r:DATA_DEF_USE]->() RETURN r
   MATCH ()-[r:CONTROLDEP_BR]->() RETURN r

Connected Nodes
^^^^^^^^^^^^^^^

Get nodes connected by edges.

**Syntax**: ``MATCH (a)-[r]->(b) RETURN a, b``

**Example**:

.. code-block:: cypher

   MATCH (a)-[r:DATA_DEF_USE]->(b) RETURN a, b
   MATCH (a)-[r:CONTROLDEP_BR]->(b) RETURN a, b

Slicing Operations
~~~~~~~~~~~~~~~~~~

Forward Slice
^^^^^^^^^^^^^

Compute forward slice from given nodes (all nodes reachable from source).

**Syntax**: ``MATCH (source)-[*]->(n) RETURN DISTINCT n``

**Example**:

.. code-block:: cypher

   MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
   WHERE input.name = 'getInput'
   MATCH (inputRet)-[*]->(n)
   RETURN DISTINCT n

**Returns**: All nodes reachable from the input nodes.

**Use Case**: Find what program elements are affected by a source.

Backward Slice
^^^^^^^^^^^^^^

Compute backward slice from given nodes (all nodes that can reach the sink).

**Syntax**: ``MATCH (n)-[*]->(sink) RETURN DISTINCT n``

**Example**:

.. code-block:: cypher

   MATCH (output:FUNC_ENTRY)-[:PARAMETER_IN]->(outputParam:PARAM_FORMALIN)
   WHERE output.name = 'printOutput'
   MATCH (n)-[*]->(outputParam)
   RETURN DISTINCT n

**Returns**: All nodes that can reach the input nodes.

**Use Case**: Find what program elements influence a sink.

Path Between Nodes
^^^^^^^^^^^^^^^^^^

Find nodes on paths between two sets of nodes.

**Syntax**: ``MATCH (start)-[*]->(end) RETURN DISTINCT end``

**Example**:

.. code-block:: cypher

   MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
   WHERE input.name = 'getInput'
   MATCH (output:FUNC_ENTRY)-[:PARAMETER_IN]->(outputParam:PARAM_FORMALIN)
   WHERE output.name = 'system'
   MATCH (inputRet)-[*]->(outputParam)
   RETURN DISTINCT outputParam

**Returns**: Sink nodes reachable from the source nodes. A non-empty result
means at least one source-to-sink path exists.

**Use Case**: Information flow analysis. Return a node from each end of the
path rather than binding ``path``; use the CLI ``--analysis=shortest-path`` or
``--analysis=chop`` modes when you need a materialized witness path.

Shortest Path
^^^^^^^^^^^^^

Shortest-path queries are a built-in analysis mode rather than a Cypher
function; ``shortestPath(...)`` is not part of the query grammar.

**Example**:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query --analysis=shortest-path \
     --criteria-query "MATCH (m:INST_FUNCALL) WHERE m.callee = 'malloc' RETURN m" \
     --target-query "MATCH (f:INST_FUNCALL) WHERE f.callee = 'free' RETURN f" \
     --edge-preset value-flow program.bc

**Returns**: The shortest dependence path between the criteria and target
nodes selected by the two Cypher queries. Add ``--format=json`` to inspect the
witness nodes or ``--format=dot`` to render the path.

Set Operations
~~~~~~~~~~~~~~~

Union
^^^^^

Union of two sets using ``UNION`` keyword.

**Syntax**: ``MATCH ... RETURN ... UNION MATCH ... RETURN ...``

**Example**:

.. code-block:: cypher

   MATCH (func1:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret1:INST_RET)
   WHERE func1.name = 'func1'
   RETURN ret1
   UNION
   MATCH (func2:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret2:INST_RET)
   WHERE func2.name = 'func2'
   RETURN ret2

**Returns**: All nodes in either set.

Intersection
^^^^^^^^^^^^

Intersection by binding the same variable in two ``MATCH`` clauses.

**Syntax**: ``MATCH (a)-[*]->(n) MATCH (b)-[*]->(n) RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (source:FUNC_ENTRY)-[*]->(n)
   WHERE source.name = 'read_input'
   MATCH (sink:FUNC_ENTRY)-[*]->(n)
   WHERE sink.name = 'process'
   RETURN n

**Returns**: Nodes reachable from both functions.

**Use Case**: Find program points shared by two flows. Bind the same variable in
both ``MATCH`` clauses to intersect their result sets.

Difference
^^^^^^^^^^

The grammar has no direct set-difference operator. Run both queries and
subtract the result sets, or exclude rows with a negated condition on a
supported property.

**Syntax**: ``MATCH (n) WHERE NOT n.property = value RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (calls:INST_FUNCALL)
   WHERE NOT calls.callee = 'printf'
   RETURN calls

**Returns**: Nodes in the first set that do not satisfy the negated condition.

Filtering with WHERE
~~~~~~~~~~~~~~~~~~~~

Property Filtering
^^^^^^^^^^^^^^^^^^

Filter nodes by properties.

**Syntax**: ``MATCH (n) WHERE n.property = value RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)
   WHERE n.name = 'main'
   RETURN n

   MATCH (n)
   WHERE n.src_line > 100 AND n.src_line < 200
   RETURN n

Multiple Conditions
^^^^^^^^^^^^^^^^^^^

Combine conditions with AND, OR, NOT.

**Syntax**: ``MATCH (n) WHERE condition1 AND condition2 RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n:INST_FUNCALL)
   WHERE n.callee = 'malloc' OR n.callee = 'calloc'
   RETURN n

Security Analysis Examples
--------------------------

Information Flow Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~

Check if secret data flows to network:

.. code-block:: cypher

   MATCH (secret:FUNC_ENTRY)-[:PARAMETER_OUT]->(secretRet:INST_RET)
   WHERE secret.name = 'getPassword'
   MATCH (network:FUNC_ENTRY)-[:PARAMETER_IN]->(networkParam:PARAM_FORMALIN)
   WHERE network.name = 'sendToNetwork'
   MATCH (secretRet)-[*]->(networkParam)
   RETURN DISTINCT networkParam

If result is non-empty, there's a potential information leak. The returned
node is the sink reached by the secret value; use ``--analysis=shortest-path``
if you need the actual witness path.

Sanitization Verification
~~~~~~~~~~~~~~~~~~~~~~~~~

Check which user inputs reach SQL sinks directly, then check which of those
flows pass through a sanitizer:

.. code-block:: cypher

   MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
   WHERE sources.name = 'getUserInput'
   MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
   WHERE sinks.name = 'executeSQL'
   MATCH (sourceRet)-[*]->(sinkParam)
   RETURN DISTINCT sinkParam

.. code-block:: cypher

   MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
   WHERE sources.name = 'getUserInput'
   MATCH (sanitizers:FUNC_ENTRY)-[:PARAMETER_OUT]->(sanitizerRet:INST_RET)
   WHERE sanitizers.name = 'sanitizeSQL'
   MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
   WHERE sinks.name = 'executeSQL'
   MATCH (sourceRet)-[*]->(sanitizerRet)-[*]->(sinkParam)
   RETURN DISTINCT sinkParam

The two queries form the two halves of a sanitization check: sinks returned by
Query 1 but not Query 2 are candidates for review. The query grammar has no
negated path pattern, so express the complement by comparing the two result
sets rather than with ``NOT EXISTS``.

Access Control Policy
~~~~~~~~~~~~~~~~~~~~~~

Check sensitive file operations guarded by an authorization check:

.. code-block:: cypher

   MATCH (auth:FUNC_ENTRY)-[:PARAMETER_OUT]->(authRet:INST_RET)
   WHERE auth.name = 'checkPermission'
   MATCH (authRet)-[:CONTROLDEP_BR]->(check)-[:CONTROLDEP_BR]->(fileParam)
   MATCH (fileOps:FUNC_ENTRY)-[:PARAMETER_IN]->(fileParam:PARAM_FORMALIN)
   WHERE fileOps.name = 'openFile' OR fileOps.name = 'writeFile'
   RETURN DISTINCT fileParam

Run the same query without the authorization chain to list every file
operation; operations missing from the guarded result need review. As with the
sanitization check, the complement is computed outside the query because
negated path patterns are not supported.

Null Pointer Analysis
~~~~~~~~~~~~~~~~~~~~~

Find potential null dereferences:

.. code-block:: cypher

   MATCH (alloc:INST_FUNCALL)
   WHERE alloc.callee = 'malloc' OR alloc.callee = 'fopen'
   MATCH (alloc)-[*]->(deref:INST_OTHER)
   WHERE deref.opcode IN ['load', 'store']
   RETURN deref

Control Flow Analysis
~~~~~~~~~~~~~~~~~~~~~

Find which conditionals affect a computation:

.. code-block:: cypher

   MATCH (branches:INST_BR)
   MATCH (computation:FUNC_ENTRY)-[:PARAMETER_IN]->(compParam:PARAM_FORMALIN)
   WHERE computation.name = 'compute'
   MATCH (branches)-[*]->(compParam)
   RETURN branches

Parameter Dependency
~~~~~~~~~~~~~~~~~~~~

Find dependencies between function parameters:

.. code-block:: cypher

   MATCH (inputs:FUNC_ENTRY)-[:PARAMETER_IN]->(inputParam:PARAM_FORMALIN)
   WHERE inputs.name = 'processData'
   MATCH (outputs:FUNC_ENTRY)-[:PARAMETER_OUT]->(outputRet:INST_RET)
   WHERE outputs.name = 'processData'
   MATCH (inputParam)-[*]->(outputRet)
   RETURN DISTINCT outputRet

Taint Analysis
~~~~~~~~~~~~~~

Complete taint analysis from sources to sinks:

.. code-block:: cypher

   MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
   WHERE sources.name = 'read' OR sources.name = 'recv' OR sources.name = 'scanf'
   MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
   WHERE sinks.name = 'system' OR sinks.name = 'exec' OR sinks.name = 'popen'
   MATCH (sourceRet)-[*]->(sinkParam)
   RETURN DISTINCT sinkParam

Query File Format
-----------------

Batch query files may contain single-line or multi-line queries:

- A line whose first character is ``#`` is a comment and also separates queries.
- Blank lines separate queries.
- A trailing ``;`` terminates the current query.
- A new ``MATCH`` after a completed query (one containing ``RETURN`` or
  ``LIMIT``) starts a new query, so files with one query per line also work.

Example query file (``security_policy.txt``):

.. code-block:: text

   # Direct flows from sources to sinks
   MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
   WHERE sources.name = 'read' OR sources.name = 'recv'
   MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
   WHERE sinks.name = 'system' OR sinks.name = 'exec'
   MATCH (sourceRet)-[*]->(sinkParam)
   RETURN DISTINCT sinkParam

   # Sensitive operations guarded by checkAuth
   MATCH (auth:FUNC_ENTRY)-[:PARAMETER_OUT]->(authRet:INST_RET)
   WHERE auth.name = 'checkAuth'
   MATCH (authRet)-[:CONTROLDEP_BR]->(check)-[:CONTROLDEP_BR]->(opParam)
   MATCH (sensitiveOps:FUNC_ENTRY)-[:PARAMETER_IN]->(opParam:PARAM_FORMALIN)
   WHERE sensitiveOps.name = 'deleteFile' OR sensitiveOps.name = 'changePassword'
   RETURN DISTINCT opParam

Run with:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -f security_policy.txt program.bc

The multi-line examples elsewhere in this page can be copied directly into a
batch file; separate each example with a blank line or ``#`` comment.

Best Practices
--------------

1. **Use Descriptive Variable Names**:

   **Good**:

   .. code-block:: cypher

      MATCH (userInput:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
      WHERE userInput.name = 'scanf'
      RETURN inputRet

   **Avoid**:

   .. code-block:: cypher

      MATCH (x:FUNC_ENTRY)-[:PARAMETER_OUT]->(y:INST_RET)
      WHERE x.name = 'scanf'
      RETURN y

2. **Break Complex Queries**:

   Use multiple MATCH clauses for clarity. This query returns input-to-sink
   flows that pass through a sanitizer:

   .. code-block:: cypher

      MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
      WHERE sources.name = 'getInput'
      MATCH (sanitizers:FUNC_ENTRY)-[:PARAMETER_OUT]->(sanitizerRet:INST_RET)
      WHERE sanitizers.name = 'sanitize'
      MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
      WHERE sinks.name = 'output'
      MATCH (sourceRet)-[*]->(sanitizerRet)-[*]->(sinkParam)
      RETURN DISTINCT sinkParam

   To find unsanitized flows, run the same query without the sanitizer pattern
   and compare the two result sets.

3. **Comment Your Policies**:

   Comments belong on their own ``#`` lines in batch files:

   .. code-block:: text

      # Policy: user input must reach the database through sanitizeSQL
      MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET) WHERE input.name = 'getUserInput' MATCH (sanitize:FUNC_ENTRY)-[:PARAMETER_OUT]->(sanitizerRet:INST_RET) WHERE sanitize.name = 'sanitizeSQL' MATCH (dbQuery:FUNC_ENTRY)-[:PARAMETER_IN]->(queryParam:PARAM_FORMALIN) WHERE dbQuery.name = 'executeQuery' MATCH (inputRet)-[*]->(sanitizerRet)-[*]->(queryParam) RETURN DISTINCT queryParam

4. **Test Incrementally**:

   Start with simple queries and build up.

   Verify the source exists:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY) WHERE n.name = 'getInput' RETURN n

   Verify the sink exists:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY) WHERE n.name = 'system' RETURN n

   Then check for flows:

   .. code-block:: cypher

      MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
      WHERE input.name = 'getInput'
      MATCH (output:FUNC_ENTRY)-[:PARAMETER_IN]->(outputParam:PARAM_FORMALIN)
      WHERE output.name = 'system'
      MATCH (inputRet)-[*]->(outputParam)
      RETURN DISTINCT outputParam

5. **Use DISTINCT for Slices**:

   .. code-block:: cypher

      MATCH (start)-[*]->(n)
      RETURN DISTINCT n

Limitations
-----------

1. **Imprecision**: PDG construction uses alias analysis which may be imprecise, leading to spurious dependencies.

2. **Scalability**: Large programs with complex dependencies may result in very large PDGs.

3. **Context Sensitivity**: PDG is context-insensitive by default. Multiple calls to the same function are merged.

4. **Array Handling**: Array dependencies may be approximated.

5. **Pointer Analysis**: Depends on underlying pointer analysis precision.

Performance Tips
----------------

1. **Limit Path Length**: Use bounded variable-length patterns when possible:

   .. code-block:: cypher

      MATCH (start)-[*1..5]->(end)
      RETURN end

2. **Use Specific Labels**: Always specify node labels when possible:

   .. code-block:: cypher

      MATCH (n:INST_FUNCALL) RETURN n

3. **Filter Early**: Apply WHERE clauses as early as possible:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY)
      WHERE n.name = 'main'
      MATCH (n)-[:PARAMETER_OUT]->(ret:INST_RET)
      RETURN ret

Troubleshooting
---------------

Query Returns Empty Set
~~~~~~~~~~~~~~~~~~~~~~~~

1. Check function names are correct (case-sensitive)
2. Verify PDG was built successfully
3. Try simpler queries first (``MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n``)
4. Use ``-v`` verbose flag

Query Too Slow
~~~~~~~~~~~~~~

1. Reduce scope of query
2. Use more specific node/edge selection
3. Limit path length with ``[*1..10]``
4. Analyze smaller program subset

Syntax Errors
~~~~~~~~~~~~~

1. Check quotes around string values: ``WHERE n.name = 'main'``
2. Ensure proper Cypher syntax
3. Balance parentheses and brackets
4. Check relationship types are correct

See Also
--------

- :doc:`../ir/pdg` - PDG construction details
- :doc:`tutorials` - PDG usage examples
- :doc:`../developer/api_reference` - Programmatic PDG access
- ``tools/ir/examples/`` - In-repo example query cookbook
- :doc:`../tools/ir/examples` - Rendered documentation for the example queries
