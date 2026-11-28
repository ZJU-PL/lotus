Loop Analysis Framework
=======================

``include/Analysis/Loop/`` and ``lib/Analysis/Loop/`` implement the loop-side
analysis stack used by optimization, profiling, and verification workflows.

**Directory structure**:

- **Root**: ``FunctionLoopAnalyses`` (per-function pass and driver),
  ``LoopContent`` (per-loop analysis container), ``LoopStructure`` and
  ``LoopForest`` (loop descriptors and nesting tree), ``LoopEnvironment``
  (live-in/live-out boundary values), and ``LoopNestingGraph`` (interprocedural
  call/loop nesting).
- **Dependence/**: ``LoopDependenceGraph``, ``LoopDependenceBuilder``,
  ``LoopSCCDAG``, and ``LoopCarriedDependencies``.
- **Scalar/**: ``LoopInvariants``, ``InductionVariable``,
  ``InductionVariables``, and ``LoopGoverningInductionVariable``.
- **Memory/**: ``LoopIterationSpaceAnalysis`` and ``MemoryCloningAnalysis``.
- **Classification/**: ``LoopCarriedVariable``, ``LoopSCCClassification``, and
  ``SCCDAGAttrs``.

Using loop facts
----------------

Start from ``FunctionLoopAnalyses`` (or ``FunctionLoopAnalysesPass``) to inspect
``LoopStructure`` and ``LoopForest``, then materialize the required stages on
each ``LoopContent``. Iteration-space and dependence information describe what is
known about a loop; transformations must still check their own legality
conditions before rewriting IR, and analyses handle loops without a recoverable
bound conservatively.

See also :doc:`cfg` and :doc:`../optimization/index`.
