SeaHorn – Verification Framework
=================================

SeaHorn is a large-scale SMT-based verification framework built on constrained
Horn clauses (CHC), symbolic execution, and abstraction-refinement.

**Binaries**: ``seahorn``, ``seapp``, ``seainspect``  
**Location**: ``tools/verifier/seahorn/``

For detailed framework documentation, see :doc:`../../../verification/seahorn`.

Overview
--------

SeaHorn performs bounded and unbounded model checking on LLVM bitcode using SMT
solvers to prove program correctness or produce counterexamples. It integrates
with Lotus analyses and solver backends to support complex verification
pipelines.

Command-Line Tools
------------------

SeaHorn Verification (seahorn)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Main verification tool for LLVM bitcode programs.

**Basic usage**:

.. code-block:: bash

   ./build/bin/seahorn [options] <input.bc>

**Common modes**:

- ``--horn-bmc`` – Bounded model checking.
- ``--horn-solve`` – CHC-based (unbounded) verification.
- ``--horn-crab`` – Use Crab/CLAM invariants during verification.

**Frequently used options**:

- ``--horn-cex=<file>`` – Dump counterexample to ``<file>``.
- ``--horn-sem-lvl=reg|ptr|mem`` – Track level for symbolic execution.
- ``--horn-format=smt2|clp|pure-smt2|mcmt`` – Format for Horn clauses.
- ``--horn-stats`` – Print verification statistics.

**Example**:

.. code-block:: bash

   # Bounded model checking
   ./build/bin/seahorn --horn-bmc program.bc

   # CHC-based verification
   ./build/bin/seahorn --horn-solve program.bc

   # Verification with Crab invariants
   ./build/bin/seahorn --horn-solve --horn-crab program.bc

SeaHorn Preprocessor (seapp)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

LLVM bitcode preprocessing tool for SeaHorn.

**Usage**:

.. code-block:: bash

   ./build/bin/seapp [options] input.bc -o output.bc

**Common options**:

- ``--horn-make-undef-warning-error`` – Treat undefined value warnings as errors
- ``--strip-extern`` – Strip external function declarations
- ``--horn-inline-all`` – Inline all functions
- ``--horn-cut-loops`` – Cut loops to make CFG acyclic

SeaHorn Inspector (seainspect)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Tool for inspecting and analyzing SeaHorn verification results.

**Usage**:

.. code-block:: bash

   ./build/bin/seainspect [options] input.bc

Counterexample Analysis
-----------------------

Generate counterexamples:

.. code-block:: bash

   ./build/bin/seahorn --horn-solve --horn-cex=cex.smt2 program.bc

Integration with Other Tools
----------------------------

SeaHorn integrates with other verifier components:

- **CLAM** – Use ``--horn-crab`` to attach Crab numerical invariants
- **Horn-ICE** – SeaHorn can emit CHC problems in SMT-LIB2 format (``--horn-format=smt2``) for verification with ``chc_verifier`` or ``hice-dt``

For more details on the SeaHorn framework architecture and components, see
:doc:`../../../verification/seahorn`.

