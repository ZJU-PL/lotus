Checker Tools
=============

This page summarizes the bug-finding tools under ``tools/checker/``. For a
feature-oriented overview and examples, see :doc:`../../user_guide/bug_detection`.

lotus_kint – Integer and Array Bug Finder
-----------------------------------------

Static analyzer focused on integer-related and array bounds bugs.

**Binary**: ``lotus_kint``  
**Location**: ``tools/checker/lotus_kint.cpp``

**Detects**:

- Integer overflow
- Division by zero
- Bad shifts
- Array out-of-bounds
- Dead branches

**Usage**:

.. code-block:: bash

   ./build/bin/lotus_kint [options] input.ll

Common options (see ``TOOLS.md`` and :doc:`../../user_guide/bug_detection`):

- ``-check-all`` – Enable all checkers
- ``-check-int-overflow`` – Integer overflow
- ``-check-div-by-zero`` – Division by zero
- ``-check-bad-shift`` – Invalid shifts
- ``-check-array-oob`` – Array out-of-bounds
- ``-check-dead-branch`` – Dead branches

lotus_gvfa – Global Value Flow Analysis
---------------------------------------

Interprocedural value-flow-based bug detector for memory safety and taint-style
issues.

**Binary**: ``lotus_gvfa``  
**Location**: ``tools/checker/lotus_gvfa.cpp``

**Detects**:

- Null pointer dereferences
- Use-after-free (via value-flow modeling)
- Taint-style flows (when configured)

**Usage**:

.. code-block:: bash

   ./build/bin/lotus_gvfa [options] input.bc

Key options:

- ``-vuln-type=nullpointer`` – Null pointer analysis (default)
- ``-vuln-type=taint`` – Taint-style vulnerability detection
- ``-test-cfl-reachability`` – Use CFL reachability for higher precision
- ``-dump-stats`` – Print analysis statistics
- ``-verbose`` – Detailed per-bug output

See :doc:`../../user_guide/bug_detection` for complete examples.

lotus_taint – Taint Analysis
----------------------------

IFDS-based interprocedural taint analysis tool.

**Binary**: ``lotus_taint``  
**Location**: ``tools/checker/lotus_taint.cpp``

**Detects**:

- Flows from untrusted sources (e.g., input) to sensitive sinks (e.g., system calls)
- Optional reaching-definitions analysis mode

**Usage**:

.. code-block:: bash

   ./build/bin/lotus_taint [options] input.bc

Key options:

- ``-analysis=0`` – Taint analysis (default)
- ``-analysis=1`` – Reaching-definitions analysis
- ``-sources=<f1,f2,...>`` – Custom source functions
- ``-sinks=<f1,f2,...>`` – Custom sink functions
- ``-max-results=<N>`` – Limit number of detailed flows
- ``-verbose`` – Detailed path information

For end-to-end examples (command injection, SQL injection, etc.), see
:doc:`../../user_guide/bug_detection`.

lotus_concur – Concurrency Bug Checker
--------------------------------------

Static analysis for data races and other concurrency issues.

**Binary**: ``lotus_concur``  
**Location**: ``tools/checker/lotus_concur.cpp``

**Detects**:

- Data races on shared variables
- Locking discipline violations
- Potential deadlocks (lock ordering issues)

**Usage**:

.. code-block:: bash

   ./build/bin/lotus_concur [options] input.bc

Typical workflow:

1. Compile multi-threaded C/C++ program to LLVM bitcode
2. Run ``lotus_concur`` on the bitcode
3. Inspect reported shared variables and threads involved in races

Detailed concurrency examples and recommended patterns are in
:doc:`../../user_guide/bug_detection`.

