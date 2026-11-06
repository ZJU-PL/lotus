Directed Fuzzing Analyses
=========================

``include/Fuzzing/Analysis/`` and ``lib/Fuzzing/Analysis/`` provide the core
distance and target-discovery analyses used by the Lotus directed fuzzing stack.

**Location**: ``include/Fuzzing/Analysis/``, ``lib/Fuzzing/Analysis/``

**Main analyses**:

- ``AFLGoBasicBlockDistanceAnalysis`` computes basic-block distances.
- ``AFLGoFunctionDistanceAnalysis`` computes function-level distances, with a
  mode used for Hawkeye-style guidance.
- ``ExtendedCallGraphAnalysis`` enriches the call graph that those distance
  analyses consume.
- ``AFLGoTargetDetectionAnalysis`` discovers target basic blocks and annotated
  target instructions.
- ``DAFLAnalysis`` reads target input and produces optional block weights.

The analysis library is built by ``lib/Fuzzing/CMakeLists.txt``, while the
compiler and linker plugin directories provide complementary instrumentation
passes documented in :doc:`aflgo_compiler` and :doc:`aflgo_linker`.

Pipeline guidance
-----------------

Run target discovery before computing distances, then feed the resulting facts
to the corresponding instrumentation stage.  Distances are guidance signals,
not proof that a target is reachable, so validate target annotations against
the program revision being fuzzed.  Recompute the analysis whenever the CFG or
call graph changes materially.

See also :doc:`fuzzing_support`.
