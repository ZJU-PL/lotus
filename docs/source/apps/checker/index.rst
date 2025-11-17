Checker Framework
==================

General-purpose bug checking utilities and analyzers for detecting various program defects.

**Location**: ``lib/Apps/Checker/``

Components
----------

* **concurrency/** – Concurrency bug detection
  * ``ConcurrencyChecker.cpp`` – Main concurrency checker coordinator
  * ``DataRaceChecker.cpp`` – Data race detection using MHP (May Happen in Parallel) analysis
  * ``DeadlockChecker.cpp`` – Deadlock detection using lock set analysis
  * ``AtomicityChecker.cpp`` – Atomicity violation detection

* **gvfa/** – Global Value Flow Analysis (GVFA) vulnerability checkers
  * ``NullPointerChecker.cpp`` – Null pointer dereference detection
  * ``UseAfterFreeChecker.cpp`` – Use-after-free detection
  * ``UseOfUninitializedVariableChecker.cpp`` – Uninitialized variable use detection
  * ``FreeOfNonHeapMemoryChecker.cpp`` – Invalid free() detection
  * ``InvalidUseOfStackAddressChecker.cpp`` – Stack address misuse detection

* **kint/** – Numerical analysis and bug detection
  * ``MKintPass.cpp`` – Main KINT pass for integer overflow, division by zero, array bounds checking
  * ``MKintPass_bugreport.cpp`` – Bug report generation for KINT
  * ``RangeAnalysis.cpp`` – Range analysis for variables
  * ``KINTTaintAnalysis.cpp`` – Taint analysis integration
  * ``BugDetection.cpp`` – Bug detection and reporting
  * ``Options.cpp`` – Command-line option parsing
  * ``Log.cpp`` – Logging utilities
  * ``Utils.cpp`` – Utility functions

* **Report/** – Bug report generation and formatting
  * ``BugReport.cpp`` – Bug report data structures
  * ``BugReportMgr.cpp`` – Centralized bug report management
  * ``BugTypes.cpp`` – Bug type definitions and classifications
  * ``SARIF.cpp`` – SARIF format output
  * ``DebugInfoAnalysis.cpp`` – Debug information extraction
  * ``ReportOptions.cpp`` – Report configuration options

Build Targets
-------------

* ``lotus-gvfa`` – Global value flow analysis tool
* ``lotus-kint`` – KINT numerical bug detection tool
* ``lotus-concur`` – Concurrency checker tool

Usage
-----

Integrated into various analysis pipelines for automated bug detection.

.. toctree::
   :maxdepth: 1

   concurrency
