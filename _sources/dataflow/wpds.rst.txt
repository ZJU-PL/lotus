WPDS Dataflow Engine
====================

Overview
========

The **WPDS engine** in ``lib/Dataflow/WPDS`` solves distributive
interprocedural data-flow problems by encoding them as
**Weighted Pushdown Systems (WPDS)**.
It computes post*/pre* reachability with the legacy ``third-party/WPDS``
implementation by default. Builds that enable ``LOTUS_ENABLE_WALI_OPENNWA``
can select the vendored WALi FWPDS or SWPDS implementation at runtime.

* **Location**: ``lib/Dataflow/WPDS``
* **Core classes**: ``InterProceduralDataFlowEngine``,
  ``GenKillTransformer``, ``DataFlowFacts``

Core Idea
=========

Instead of building an exploded super-graph (as IFDS/IDE do), the WPDS
engine:

* constructs a pushdown system whose rules correspond to CFG edges and
  call/return transitions,
* attaches **weights** that model GEN/KILL behavior on a set of facts,
* runs a standard WPDS algorithm (post* or pre*) to compute the least
  fixed point.

Clients typically provide a function
``GenKillTransformer *createTransformer(Instruction *I)`` which
captures the effect of each instruction on the fact set.

Backend selection and prepared sessions
=======================================

``WPDSBackendOptions::backend`` accepts ``Legacy``, ``WaliFWPDS``, or
``WaliSWPDS``. The default remains ``Legacy``. An explicit WALi selection in a
build without WALi fails with a diagnostic naming the enabling CMake option; it
does not silently fall back.

``prepareForwardAnalysis`` and ``prepareBackwardAnalysis`` lower the module and
compile the chosen backend once. Calls to ``PreparedAnalysis::solve`` may then
change the boundary fact seed while reusing that preparation. SWPDS performs
its vendor ``preprocess()`` step once per prepared session. The prepared object,
and any returned instruction-indexed result, require the LLVM module to remain
alive.

The normal solve retains the legacy exact-stack observation. Use
``solveContextAggregated`` to accept a program-point symbol followed by any
call-stack suffix and aggregate observations inside callees or recursive calls.
``verifyAgainstLegacy`` independently solves the same frozen model and compares
reachability and materialized fact values at every observation.

Example Analyses
================

Constant Propagation
--------------------

``WPDSConstantPropagation`` demonstrates a forward **constant
propagation** analysis:

* facts track variables that currently hold constant values,
* GEN adds variables whose values are known to be constant at a point,
* KILL removes variables whose values become non-constant,
* the interprocedural engine propagates these facts across calls.

Liveness
--------

``WPDSLivenessAnalysis`` performs a backward **liveness analysis**:

* GEN collects variables used at an instruction (including loads and
  uses in stores),
* KILL records variables defined (or overwritten) at that point,
* the WPDS formulation makes the analysis naturally interprocedural.

Taint Analysis
--------------

``WPDSTaintAnalysis`` is a WPDS-based **taint analysis**:

* facts represent tainted values,
* GEN introduces taint at configurable source calls,
* KILL removes taint at sanitizers,
* the engine reports flows of tainted data into dangerous sinks
  (e.g., ``system``, ``exec``, ``strcpy``).

Uninitialized Variables
-----------------------

``WPDSUninitializedVariables`` shows a forward analysis for detecting
reads from **possibly uninitialized memory**:

* GEN marks newly allocated locals (``alloca``) as uninitialized,
* KILL removes a location when it is definitely initialized by a store,
* any load whose pointer appears in the ``IN`` set is reported as a
  potential use of uninitialized data.

Compared to IFDS/IDE, the explicit pushdown-system encoding makes the
call stack and recursion behavior more visible and can be advantageous
for certain classes of interprocedural problems.

.. note::

   The public backend and prepared-session headers do not expose WALi types.
   Caller-provided legacy configuration automata and witness queries remain
   legacy-only expert interfaces.
