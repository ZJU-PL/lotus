Fuzzing Support
================

Lotus provides directed greybox fuzzing analyses and support code for target
driven instrumentation.

Overview
--------

The fuzzing tree implements directed fuzzing analyses based on AFLGo,
Hawkeye, and DAFL in a modular form. The core analyses live under
``lib/Fuzzing/Analysis/``, with complementary compile-time and link-time
plugins under ``lib/Fuzzing/AFLGoCompiler/`` and ``lib/Fuzzing/AFLGoLinker/``.

**Location**: ``lib/Fuzzing/``, ``include/Fuzzing/``

**Components**: distance analyses, target detection, target generation,
path profiling, and AFLGo compiler and linker plugin code.

Detailed module pages:

- :doc:`fuzzing_analysis`
- :doc:`aflgo_compiler`
- :doc:`aflgo_linker`

Directed Greybox Fuzzing Algorithms
------------------------------------

**AFLGo (CCS 17)**: Distance-based guidance using function and basic-block
distances.

**Hawkeye (CCS 18)**: Function-level distance analysis.

**DAFL (USENIX Security 23)**: Data-dependence guided weighting.

Distance Analysis
-----------------

The ``Analysis/`` directory provides the main reusable analyses:

* **BasicBlockDistance.cpp**
  * Implements ``AFLGoBasicBlockDistanceAnalysis``
  * Computes basic-block distances using function-distance results

* **FunctionDistance.cpp**
  * Implements ``AFLGoFunctionDistanceAnalysis``
  * Computes function-level distances for AFLGo and Hawkeye-style guidance

* **DAFL.cpp**
  * Implements ``DAFLAnalysis``
  * Computes optional basic-block weights from input target data

* **ExtendedCallGraphAnalysis.cpp**
  * Implements ``ExtendedCallGraphAnalysis``
  * Enriches the call graph used by the distance analyses

* **TargetDetection.cpp**
  * Implements ``AFLGoTargetDetectionAnalysis``
  * Finds target basic blocks and annotated instructions

* **TargetGeneration.cpp**
  * Shared support code in ``lib/Fuzzing/`` for generating fuzzing targets

Public headers for this layer live under ``include/Fuzzing/Analysis/``.

Compiler and Linker Plugins
---------------------------

``AFLGoCompiler/`` and ``AFLGoLinker/`` provide compile-time and link-time
plugins for target injection and distance instrumentation.

**PathProfiling/** (``lib/Fuzzing/PathProfiling/``, ``include/Fuzzing/PathProfiling/``):

Ball-Larus path numbering for path-aware fuzzing. Assigns unique path numbers
to paths through a CFG DAG (after backedge removal) so that individual
execution paths can be identified, counted, and targeted during fuzzing.

**AFLGoCompiler/**: LLVM compiler plugin sources for compile-time target injection:

* ``Plugin.cpp`` – LLVM plugin entry point for AFLGo instrumentation
* ``TargetInjection.cpp`` – Target injection for directed fuzzing

**AFLGoLinker/**: LLVM linker plugin sources for link-time instrumentation:

* ``Plugin.cpp`` – Link-time instrumentation plugin
* ``DAFL.cpp`` – DAFL-specific instrumentation
* ``DistanceInstrumentation.cpp`` – Distance-based instrumentation
* ``FunctionDistanceInstrumentation.cpp`` – Function-level distance instrumentation
* ``DuplicateTargetRemoval.cpp`` – Target deduplication
* ``TargetInjectionFixup.cpp`` – Target injection fixup

Target Detection
----------------

Automatic target identification support for the directed fuzzing pipeline.

The target-detection interface is exposed through
``include/Fuzzing/Analysis/TargetDetection.h``.
