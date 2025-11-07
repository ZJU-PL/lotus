SMT-Based Model Checking (SeaHorn)
===================================

SeaHorn provides SMT-based verification using constrained Horn clauses (CHC) and symbolic execution.

Overview
--------

SeaHorn performs bounded model checking and verification using SMT solvers to prove correctness or find counterexamples.

**Location**: ``tools/verifier/seahorn``, ``tools/verifier/horn-ice``

**Components**: SeaHorn (symbolic execution), Horn-ICE (CHC solving), Sea-rt (counterexamples)

SeaHorn Verification
--------------------

Main verification tool for C programs.

**Usage**:
.. code-block:: bash

   ./build/bin/seahorn [options] <input.c>

**Modes**: ``--bmc=<N>`` (bounded), ``--horn`` (unbounded), ``--abstractor=clam`` (abstract)

**Options**: ``--cex=<file>``, ``--track=mem``, ``--horn-solver=spacer|ice``

Counterexample Analysis
~~~~~~~~~~~~~~~~~~~~~~~

Generate executable counterexamples:

.. code-block:: bash

   ./build/bin/seahorn --cex=harness.ll program.c
   clang -m64 -g program.c harness.ll -o counterexample

Horn-ICE CHC Verification
-------------------------

CHC verification with invariant learning.

**Usage**:
.. code-block:: bash

   ./build/bin/chc-verifier <input.smt2>
   ./build/bin/hice-dt <input.smt2>  # With learning

**CHC Format**: SMT-LIB2 with Horn clauses

Verification Workflow
---------------------

1. Compile: ``clang -c -emit-llvm program.c -o program.bc``
2. Verify: ``./build/bin/seahorn program.c``
3. Debug: ``./build/bin/seahorn --cex=harness.ll program.c``

Solver Guide
------------

| Solver | Strengths | Best For |
|--------|-----------|----------|
| Spacer | Fast, reliable | General verification |
| Horn-ICE | Invariant learning | Complex invariants (?) |
| CLAM | Abstract domains | Numerical properties |
