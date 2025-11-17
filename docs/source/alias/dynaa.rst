========================
DynAA — Dynamic Alias AA
========================

Overview
========

DynAA provides **dynamic validation** for static alias analyses. It
instruments programs to record runtime points-to information, then compares
these logs against the results of a chosen static AA.

* **Static side**: any Lotus AA (e.g., Andersen, AserPTA, LotusAA)
* **Dynamic side**: runtime traces collected by DynAA
* **Location**: ``lib/Alias/Dynamic`` and tools under ``tools/dynaa``

Workflow
========

The typical workflow is:

1. **Instrument** the bitcode:

   .. code-block:: bash

      ./build/bin/dynaa-instrument example.bc -o example.inst.bc

2. **Compile** and link with the runtime:

   .. code-block:: bash

      clang example.inst.bc libRuntime.a -o example.inst

3. **Run** the instrumented binary to produce logs:

   .. code-block:: bash

      LOG_DIR=logs/ ./example.inst

4. **Check** a static AA against the logs:

   .. code-block:: bash

      ./build/bin/dynaa-check example.bc logs/pts.log basic-aa

Features and Use Cases
======================

* Detects mismatches between static AA predictions and observed behavior.
* Helps evaluate **precision** and **soundness** of new alias analyses.
* Useful for regression testing and benchmarking AA implementations.






