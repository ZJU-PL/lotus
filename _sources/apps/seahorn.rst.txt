SeaHorn Verification Framework
==============================

Large-scale SMT-based verification framework built on constrained Horn clauses
(CHC), symbolic execution, and abstraction-refinement.

Overview
--------

SeaHorn performs bounded and unbounded model checking on LLVM bitcode using SMT
solvers to prove program correctness or produce counterexamples. It integrates
with Lotus analyses and solver backends to support complex verification
pipelines.

**Location**: ``lib/Apps/seahorn/``

**Build Targets**:

- ``tools/seahorn``
- ``tools/verifier/seahorn/seahorn``
- ``tools/verifier/seahorn/seapp``
- ``tools/verifier/seahorn/seainspect``
- ``tools/verifier/horn-ice/chc_verifier``
- ``tools/verifier/horn-ice/hice-dt``

Structure
---------

The SeaHorn framework is organized into the following subdirectories:

* **seahorn/** – Core SeaHorn verification engine
  * Horn clause generation and solving (HornifyFunction, HornifyModule)
  * Symbolic execution engines (BvOpSem, ClpOpSem, UfoOpSem)
  * Bounded model checking (Bmc, PathBmc)
  * Counterexample generation (CexHarness, CexExeGenerator)
  * **Smt/** – SMT solver integration (Z3, Yices2)

* **Analysis/** – Analysis passes for verification
  * ``CrabAnalysis.cc`` – CRAB abstract interpretation integration
  * ``CanAccessMemory.cc`` – Memory access analysis
  * ``CanFail.cc`` – Failure point analysis
  * ``CanReadUndef.cc`` – Undefined value read analysis
  * ``ClassHierarchyAnalysis.cc`` – C++ class hierarchy analysis
  * ``ControlDependenceAnalysis.cc`` – Control dependence computation
  * ``CutPointGraph.cc`` – Cut point graph construction
  * ``GateAnalysis.cc`` – Gate analysis for verification
  * ``StaticTaint.cc`` – Static taint analysis
  * ``TopologicalOrder.cc``, ``WeakTopologicalOrder.cc`` – Ordering analyses

* **Transforms/** – LLVM IR transformations for verification
  * **Instrumentation/** – Property instrumentation
    * ``BufferBoundsCheck.cc``, ``FatBufferBoundsCheck.cc`` – Buffer bounds checking
    * ``NullCheck.cc`` – Null pointer checking
    * ``SimpleMemoryCheck.cc`` – Memory safety checks
    * ``MixedSemantics.cc`` – Mixed operational semantics
  * **Scalar/** – Scalar optimizations
    * ``CutLoops.cc`` – Loop cutting
    * ``LoopPeeler.cc`` – Loop peeling
    * ``LowerCstExpr.cc`` – Constant expression lowering
    * ``LowerGvInitializers.cc`` – Global variable initializer lowering
    * ``PromoteVerifierCalls.cc`` – Verifier call promotion
    * ``BackedgeCutter.cc`` – Backedge cutting
  * **Utils/** – Transformation utilities
    * ``DevirtFunctions.cc`` – Function devirtualization
    * ``ExternalizeFunctions.cc`` – Function externalization
    * ``Mem2Reg.cc`` – Memory-to-register promotion
    * ``SliceFunctions.cc`` – Function slicing

* **Support/** – Utility functions
  * ``CFGPrinter.cc`` – Control flow graph printing
  * ``GitSHA1.cc`` – Version information
  * ``Profiler.cc`` – Profiling support
  * ``Stats.cc`` – Statistics collection

Components
----------

- **SeaHorn** – LLVM-based front-end, symbolic execution, and encoding of
  verification problems as CHCs.
- **Horn-ICE** – CHC solving with invariant learning (in ``tools/verifier/horn-ice/``).
- **Sea-rt** – Runtime components for executing counterexample harnesses (in ``tools/verifier/seahorn/sea-rt/``).

SeaHorn Verification
--------------------

Main verification tool for C programs.

**Basic usage**:

.. code-block:: bash

   ./build/bin/seahorn [options] <input.c>

**Common modes**:

- ``--bmc=<N>`` – Bounded model checking up to ``N`` steps.
- ``--horn`` – CHC-based (unbounded) verification.
- ``--abstractor=clam`` – Use CLAM-based abstract interpretation as an
  abstractor.

**Frequently used options**:

- ``--cex=<file>`` – Dump a counterexample harness to ``<file>``.
- ``--track=mem`` – Track memory (heap/stack) explicitly.
- ``--horn-solver=spacer|ice`` – Select CHC solving engine.

Counterexample Analysis
~~~~~~~~~~~~~~~~~~~~~~~

Generate executable counterexamples:

.. code-block:: bash

   ./build/bin/seahorn --cex=harness.ll program.c
   clang -m64 -g program.c harness.ll -o counterexample

