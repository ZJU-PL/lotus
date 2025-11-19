PDG Query Language
==================

The Program Dependence Graph (PDG) Query Language allows you to query dependencies, perform slicing, and verify security policies on program dependence graphs.

Overview
--------

The PDG query language provides a declarative way to:

- Query program dependencies (data and control)
- Perform forward and backward slicing
- Check information flow properties
- Verify security policies
- Find shortest paths between program elements
- Analyze parameter dependencies

The language supports:

- Set operations (union, intersection, difference)
- Node and edge selection
- Path queries
- Policy constraints
- Variable binding

Getting Started
---------------

Building the PDG
~~~~~~~~~~~~~~~~

First, compile your program and build the PDG:

.. code-block:: bash

   # Compile to LLVM IR
   clang -emit-llvm -g -c program.c -o program.bc
   
   # Run PDG query tool
   ./build/bin/pdg-query program.bc

Interactive Mode
~~~~~~~~~~~~~~~~

Use ``-i`` flag for interactive queries:

.. code-block:: bash

   ./build/bin/pdg-query -i program.bc
   
   PDG> returnsOf("main")
   [Results displayed]
   
   PDG> forwardSlice(returnsOf("getInput"))
   [Slice displayed]
   
   PDG> exit

Single Query Mode
~~~~~~~~~~~~~~~~~

Use ``-q`` flag for single query:

.. code-block:: bash

   ./build/bin/pdg-query -q "returnsOf(\"main\")" program.bc

Batch Query Mode
~~~~~~~~~~~~~~~~

Use ``-f`` flag to run queries from file:

.. code-block:: bash

   ./build/bin/pdg-query -f queries.txt program.bc

Policy Check Mode
~~~~~~~~~~~~~~~~~

Use ``-p`` flag for policy verification:

.. code-block:: bash

   ./build/bin/pdg-query -p "noExplicitFlows(sources, sinks) is empty" program.bc

Language Reference
------------------

Basic Expressions
~~~~~~~~~~~~~~~~~

``pgm``
^^^^^^^

Returns the entire program dependence graph.

**Example**:

.. code-block:: text

   pgm

**Returns**: All nodes in the PDG.

``returnsOf(functionName)``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Returns the return values of a function.

**Syntax**: ``returnsOf("functionName")``

**Example**:

.. code-block:: text

   returnsOf("main")
   returnsOf("getInput")

**Returns**: Nodes representing return values of the specified function.

``formalsOf(functionName)``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Returns the formal parameters of a function.

**Syntax**: ``formalsOf("functionName")``

**Example**:

.. code-block:: text

   formalsOf("process_data")

**Returns**: Nodes representing formal input parameters.

``entriesOf(functionName)``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Returns the entry points of a function.

**Syntax**: ``entriesOf("functionName")``

**Example**:

.. code-block:: text

   entriesOf("main")

**Returns**: Function entry nodes.

``actualsOf(callSite)``
^^^^^^^^^^^^^^^^^^^^^^^

Returns actual parameters at a call site.

**Syntax**: ``actualsOf(callSiteExpr)``

**Example**:

.. code-block:: text

   actualsOf(selectNodes(INST_FUNCALL))

**Returns**: Actual parameter nodes at call sites.

Node Selection
~~~~~~~~~~~~~~

``selectNodes(nodeType)``
^^^^^^^^^^^^^^^^^^^^^^^^^

Select all nodes of a specific type.

**Syntax**: ``selectNodes(TYPE)``

**Node Types**:

- ``INST_FUNCALL`` - Function call instructions
- ``INST_RET`` - Return instructions
- ``INST_BR`` - Branch instructions
- ``INST_LOAD`` - Load instructions
- ``INST_STORE`` - Store instructions
- ``INST_ALLOCA`` - Alloca instructions
- ``INST_GEP`` - GetElementPtr instructions
- ``INST_OTHER`` - Other instructions
- ``FUNC_ENTRY`` - Function entry points
- ``PARAM_FORMALIN`` - Formal input parameters
- ``PARAM_FORMALOUT`` - Formal output parameters (return values)
- ``PARAM_ACTUALIN`` - Actual input parameters
- ``PARAM_ACTUALOUT`` - Actual output parameters
- ``GLOBAL`` - Global variables
- ``FUNC`` - Function nodes
- ``CLASS`` - Class nodes

**Example**:

.. code-block:: text

   selectNodes(INST_FUNCALL)
   selectNodes(INST_BR)

Edge Selection
~~~~~~~~~~~~~~

``selectEdges(edgeType)``
^^^^^^^^^^^^^^^^^^^^^^^^^

Select all edges of a specific type.

**Syntax**: ``selectEdges(TYPE)``

**Edge Types**:

- ``DATA_DEF_USE`` - Data definition-use edges
- ``DATA_RAW`` - Read-after-write edges
- ``DATA_READ`` - Read edges
- ``DATA_WRITE`` - Write edges
- ``DATA_ALIAS`` - Alias edges
- ``CONTROL_DEP`` - Control dependency edges
- ``CONTROLDEP_BR`` - Control dependency from branches
- ``CONTROLDEP_ENTRY`` - Control dependency from entry
- ``PARAMETER_IN`` - Parameter input edges
- ``PARAMETER_OUT`` - Parameter output edges
- ``PARAMETER_FIELD`` - Field parameter edges
- ``CALL_DEP`` - Call dependency edges
- ``GLOBAL_DEP`` - Global dependency edges

**Example**:

.. code-block:: text

   selectEdges(DATA_DEF_USE)
   selectEdges(CONTROL_DEP)

Slicing Operations
~~~~~~~~~~~~~~~~~~

``forwardSlice(expr)``
^^^^^^^^^^^^^^^^^^^^^^

Compute forward slice from given nodes.

**Syntax**: ``forwardSlice(nodeExpr)``

**Example**:

.. code-block:: text

   forwardSlice(returnsOf("getInput"))

**Returns**: All nodes reachable from the input nodes.

**Use Case**: Find what program elements are affected by a source.

``backwardSlice(expr)``
^^^^^^^^^^^^^^^^^^^^^^^

Compute backward slice from given nodes.

**Syntax**: ``backwardSlice(nodeExpr)``

**Example**:

.. code-block:: text

   backwardSlice(formalsOf("printOutput"))

**Returns**: All nodes that can reach the input nodes.

**Use Case**: Find what program elements influence a sink.

``between(expr1, expr2)``
^^^^^^^^^^^^^^^^^^^^^^^^^

Find nodes on paths between two sets of nodes.

**Syntax**: ``between(sourceExpr, sinkExpr)``

**Example**:

.. code-block:: text

   between(returnsOf("getInput"), formalsOf("system"))

**Returns**: Nodes on any path from sources to sinks.

**Use Case**: Information flow analysis.

``shortestPath(expr1, expr2)``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Find shortest path between two sets of nodes.

**Syntax**: ``shortestPath(sourceExpr, sinkExpr)``

**Example**:

.. code-block:: text

   shortestPath(returnsOf("malloc"), formalsOf("free"))

**Returns**: Nodes on shortest path.

Set Operations
~~~~~~~~~~~~~~

Union (``U``)
^^^^^^^^^^^^^

Union of two sets.

**Syntax**: ``expr1 U expr2``

**Example**:

.. code-block:: text

   returnsOf("func1") U returnsOf("func2")
   selectNodes(INST_LOAD) U selectNodes(INST_STORE)

**Returns**: All nodes in either set.

Intersection (``∩`` or ``^``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Intersection of two sets.

**Syntax**: ``expr1 ∩ expr2`` or ``expr1 ^ expr2``

**Example**:

.. code-block:: text

   forwardSlice(sources) ∩ backwardSlice(sinks)

**Returns**: Nodes in both sets.

**Use Case**: Find nodes that are both sources and sinks.

Difference (``-``)
^^^^^^^^^^^^^^^^^^

Set difference.

**Syntax**: ``expr1 - expr2``

**Example**:

.. code-block:: text

   pgm - returnsOf("main")

**Returns**: Nodes in first set but not in second.

Variable Binding
~~~~~~~~~~~~~~~~

``let var = expr1 in expr2``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Bind a subexpression to a variable.

**Syntax**: ``let varName = expr1 in expr2``

**Example**:

.. code-block:: text

   let sources = returnsOf("getInput") in
   let sinks = formalsOf("system") in
   between(sources, sinks)

**Use Case**: Improve readability and reuse subexpressions.

Policy Checks
~~~~~~~~~~~~~

``expr is empty``
^^^^^^^^^^^^^^^^^

Check if expression evaluates to empty set.

**Syntax**: ``expr is empty``

**Example**:

.. code-block:: text

   between(secret, public) is empty

**Returns**: True if set is empty, false otherwise.

**Use Case**: Verify that no path exists (security policy).

``expr is not empty``
^^^^^^^^^^^^^^^^^^^^^

Check if expression evaluates to non-empty set.

**Syntax**: ``expr is not empty``

**Example**:

.. code-block:: text

   forwardSlice(input) is not empty

**Returns**: True if set is non-empty, false otherwise.

Security Analysis Functions
~~~~~~~~~~~~~~~~~~~~~~~~~~~

``noExplicitFlows(sources, sinks)``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Check for explicit information flows from sources to sinks.

**Syntax**: ``noExplicitFlows(sourceExpr, sinkExpr)``

**Example**:

.. code-block:: text

   noExplicitFlows(returnsOf("getSecret"), formalsOf("printf"))

**Returns**: Paths from sources to sinks (empty if no flows).

**Use Case**: Confidentiality policy - secrets should not flow to public outputs.

``declassifies(sanitizers, sources, sinks)``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Check if all flows from sources to sinks go through sanitizers.

**Syntax**: ``declassifies(sanitizerExpr, sourceExpr, sinkExpr)``

**Example**:

.. code-block:: text

   let input = returnsOf("scanf")
   let sanitize = returnsOf("sanitize_input")
   let output = formalsOf("system")
   declassifies(sanitize, input, output)

**Returns**: Flows that bypass sanitizers.

**Use Case**: Verify sanitization policies.

``accessControlled(checks, operations)``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Check if sensitive operations are guarded by authorization checks.

**Syntax**: ``accessControlled(checkExpr, operationExpr)``

**Example**:

.. code-block:: text

   let authChecks = returnsOf("isAuthorized")
   let sensitiveOps = selectNodes(INST_FUNCALL)
   accessControlled(authChecks, sensitiveOps)

**Returns**: Unguarded operations.

**Use Case**: Access control policy verification.

Examples
--------

Example 1: Basic Dependency Query
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Find what depends on user input:

.. code-block:: text

   let input = returnsOf("scanf")
   forwardSlice(input)

Example 2: Information Flow Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Check if secret data flows to network:

.. code-block:: text

   let secret = returnsOf("getPassword")
   let network = formalsOf("sendToNetwork")
   between(secret, network)

If result is non-empty, there's a potential information leak.

Example 3: Sanitization Verification
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Verify all user inputs are sanitized before use in SQL:

.. code-block:: text

   let sources = returnsOf("getUserInput")
   let sanitizers = returnsOf("sanitizeSQL")
   let sinks = formalsOf("executeSQL")
   declassifies(sanitizers, sources, sinks) is empty

If result is true, all flows are sanitized.

Example 4: Access Control Policy
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Verify sensitive file operations require authorization:

.. code-block:: text

   let authCheck = returnsOf("checkPermission")
   let fileOps = formalsOf("openFile") U formalsOf("writeFile")
   accessControlled(authCheck, fileOps) is empty

Example 5: Null Pointer Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Find potential null dereferences:

.. code-block:: text

   let nullReturns = returnsOf("malloc") U returnsOf("fopen")
   let dereferences = selectNodes(INST_LOAD) U selectNodes(INST_STORE)
   forwardSlice(nullReturns) ∩ dereferences

Example 6: Control Flow Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Find which conditionals affect a computation:

.. code-block:: text

   let branches = selectNodes(INST_BR)
   let computation = formalsOf("compute")
   backwardSlice(computation) ∩ branches

Example 7: Parameter Dependency
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Find dependencies between function parameters:

.. code-block:: text

   let inputs = formalsOf("processData")
   let outputs = returnsOf("processData")
   between(inputs, outputs)

Example 8: Taint Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~

Complete taint analysis from sources to sinks:

.. code-block:: text

   let sources = returnsOf("read") U returnsOf("recv") U returnsOf("scanf")
   let sinks = formalsOf("system") U formalsOf("exec") U formalsOf("popen")
   let flows = between(sources, sinks)
   flows is not empty

Example 9: Multi-hop Flows
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Find flows that go through an intermediate function:

.. code-block:: text

   let source = returnsOf("getInput")
   let intermediate = formalsOf("process")
   let sink = formalsOf("output")
   let path1 = between(source, intermediate)
   let path2 = between(intermediate, sink)
   (path1 ∩ path2) is not empty

Example 10: Complex Policy
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Verify that:
1. User input is sanitized
2. Authorization is checked
3. Then operation is performed

.. code-block:: text

   let input = returnsOf("getUserInput")
   let sanitize = returnsOf("sanitize")
   let authCheck = returnsOf("authorize")
   let operation = formalsOf("performOperation")
   
   let sanitizedInput = between(input, sanitize)
   let authorizedOp = between(authCheck, operation)
   let sanitizedToOp = between(sanitize, operation)
   
   (sanitizedInput is not empty) and
   (authorizedOp is not empty) and
   (sanitizedToOp is not empty)

Query File Format
-----------------

Batch query files support:

- Comments (``#`` prefix)
- Multiple queries (one per line or separated by ``;``)
- Variable definitions
- Policy checks

Example query file (``security_policy.txt``):

.. code-block:: text

   # Security Policy Verification
   
   # Define sources
   let sources = returnsOf("read") U returnsOf("recv")
   
   # Define sinks
   let sinks = formalsOf("system") U formalsOf("exec")
   
   # Check 1: No direct flows from sources to sinks
   between(sources, sinks) is empty
   
   # Check 2: All sensitive operations are authorized
   let authChecks = returnsOf("checkAuth")
   let sensitiveOps = formalsOf("deleteFile") U formalsOf("changePassword")
   accessControlled(authChecks, sensitiveOps) is empty

Run with:

.. code-block:: bash

   ./build/bin/pdg-query -f security_policy.txt program.bc

Visualization
-------------

Generate DOT File
~~~~~~~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/pdg-query -dump-dot program.bc

This creates ``pdg.dot`` file.

Convert to PDF
~~~~~~~~~~~~~~

.. code-block:: bash

   dot -Tpdf pdg.dot -o pdg.pdf

Visualize Query Results
~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/pdg-query -q "forwardSlice(returnsOf(\"main\"))" \
                          -dump-subgraph=result.dot \
                          program.bc
   dot -Tpdf result.dot -o result.pdf

Best Practices
--------------

1. **Use Descriptive Variable Names**:

   .. code-block:: text

      # Good
      let userInputs = returnsOf("scanf")
      
      # Bad
      let x = returnsOf("scanf")

2. **Break Complex Queries**:

   .. code-block:: text

      # Good
      let sources = returnsOf("getInput")
      let sanitizers = returnsOf("sanitize")
      let sinks = formalsOf("output")
      declassifies(sanitizers, sources, sinks)
      
      # Bad
      declassifies(returnsOf("sanitize"), returnsOf("getInput"), formalsOf("output"))

3. **Comment Your Policies**:

   .. code-block:: text

      # Policy: User input must be sanitized before database queries
      let input = returnsOf("getUserInput")
      let sanitize = returnsOf("sanitizeSQL")
      let dbQuery = formalsOf("executeQuery")
      declassifies(sanitize, input, dbQuery) is empty

4. **Test Incrementally**:

   Start with simple queries and build up:

   .. code-block:: text

      # Step 1: Verify sources exist
      returnsOf("getInput")
      
      # Step 2: Verify sinks exist
      formalsOf("system")
      
      # Step 3: Check flows
      between(returnsOf("getInput"), formalsOf("system"))

5. **Use Set Operations Efficiently**:

   .. code-block:: text

      # Efficient: Compute once
      let allCalls = selectNodes(INST_FUNCALL)
      forwardSlice(allCalls)
      
      # Inefficient: Compute multiple times
      forwardSlice(selectNodes(INST_FUNCALL))
      backwardSlice(selectNodes(INST_FUNCALL))

Limitations
-----------

1. **Imprecision**: PDG construction uses alias analysis which may be imprecise, leading to spurious dependencies.

2. **Scalability**: Large programs with complex dependencies may result in very large PDGs.

3. **Context Sensitivity**: PDG is context-insensitive by default. Multiple calls to the same function are merged.

4. **Array Handling**: Array dependencies may be approximated.

5. **Pointer Analysis**: Depends on underlying pointer analysis precision.

Advanced Topics
---------------

Custom Predicates
~~~~~~~~~~~~~~~~~

You can define custom node predicates (requires code changes):

.. code-block:: cpp

   // In PDG query implementation
   bool customPredicate(Node* n) {
       // Custom logic
       return /* condition */;
   }
   
   // Use in queries
   selectNodes(CUSTOM_PREDICATE)

Extending the Query Language
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

See :doc:`../developer/developer_guide` for how to add new query operations.

Integration with Other Tools
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

PDG query results can be exported and used in other analyses:

.. code-block:: bash

   # Export to JSON
   ./build/bin/pdg-query -q "query" -output-json=results.json program.bc
   
   # Process with Python
   python analyze_results.py results.json

Performance Tips
----------------

1. **Precompute Common Subexpressions**:

   Use ``let`` to avoid recomputation.

2. **Limit Scope**:

   Query specific functions rather than entire program:

   .. code-block:: text

      # Instead of
      forwardSlice(pgm)
      
      # Do
      forwardSlice(returnsOf("specificFunction"))

3. **Use Appropriate Edge Types**:

   Select only relevant edge types to reduce graph size:

   .. code-block:: text

      selectEdges(DATA_DEF_USE)  # Only data dependencies

Troubleshooting
---------------

Query Returns Empty Set
~~~~~~~~~~~~~~~~~~~~~~~

1. Check function names are correct (case-sensitive)
2. Verify PDG was built successfully
3. Try simpler queries first (``returnsOf("main")``)
4. Use ``-v`` verbose flag

Query Too Slow
~~~~~~~~~~~~~~

1. Reduce scope of query
2. Use more specific node/edge selection
3. Analyze smaller program subset

Syntax Errors
~~~~~~~~~~~~~

1. Check quotes around function names: ``returnsOf("func")``
2. Ensure operators are correct: ``U`` for union, ``∩`` for intersection
3. Balance parentheses

See Also
--------

- :doc:`ir/intermediate_representations` - PDG construction details
- :doc:`tutorials` - PDG usage examples
- :doc:`../developer/api_reference` - Programmatic PDG access
- ``examples/pdg-queries/`` - More example queries

