Checker Framework
==================

The Checker Framework provides a unified infrastructure for static bug detection across multiple vulnerability categories. It integrates various analysis techniques to detect security vulnerabilities, concurrency bugs, and numerical errors in LLVM bitcode.

**Location**: ``lib/Checker/``

**Headers**: ``include/Checker/``

**Tools**: ``tools/checker/`` (command-line frontends)

Overview
--------

The Checker Framework consists of three main checker categories, all unified through a centralized bug reporting system:

* **GVFA Checkers** – Memory safety vulnerabilities using Global Value Flow Analysis
* **KINT Checkers** – Numerical bugs (overflow, division by zero, array bounds) using range analysis and SMT solving
* **Concurrency Checkers** – Thread safety issues (data races, deadlocks, atomicity violations) using MHP and lock set analysis

All checkers report bugs through the centralized ``BugReportMgr`` system, enabling unified output formats (JSON, SARIF) and consistent bug reporting across all analysis tools.

Components
----------

**Concurrency Checkers** (``lib/Checker/Concurrency/``):

* ``ConcurrencyChecker.cpp`` – Main concurrency checker coordinator
* ``DataRaceChecker.cpp`` – Data race detection using MHP (May Happen in Parallel) analysis
* ``DeadlockChecker.cpp`` – Deadlock detection using lock set analysis
* ``AtomicityChecker.cpp`` – Atomicity violation detection

**GVFA Vulnerability Checkers** (``lib/Checker/GVFA/``):

* ``NullPointerChecker.cpp`` – Null pointer dereference detection (CWE-476, CWE-690)
* ``UseAfterFreeChecker.cpp`` – Use-after-free detection (CWE-416)
* ``UseOfUninitializedVariableChecker.cpp`` – Uninitialized variable use detection (CWE-457)
* ``FreeOfNonHeapMemoryChecker.cpp`` – Invalid free() detection (CWE-590)
* ``InvalidUseOfStackAddressChecker.cpp`` – Stack address misuse detection (CWE-562)
* ``GVFAVulnerabilityChecker.h`` – Base interface for all GVFA checkers

**KINT Numerical Checkers** (``lib/Checker/KINT/``):

* ``MKintPass.cpp`` – Main KINT pass for integer overflow, division by zero, array bounds checking
* ``MKintPass_bugreport.cpp`` – Bug report generation for KINT
* ``RangeAnalysis.cpp`` – Range analysis for variables using abstract interpretation
* ``KINTTaintAnalysis.cpp`` – Taint analysis integration for tracking untrusted data
* ``BugDetection.cpp`` – Bug detection and reporting logic
* ``Options.cpp`` – Command-line option parsing
* ``Log.cpp`` – Logging utilities
* ``Utils.cpp`` – Utility functions

**Report System** (``lib/Checker/Report/``):

* ``BugReport.cpp`` – Bug report data structures with source location information
* ``BugReportMgr.cpp`` – Centralized bug report management (singleton pattern)
* ``BugTypes.cpp`` – Bug type definitions, classifications, and CWE mappings
* ``SARIF.cpp`` – SARIF format output support
* ``DebugInfoAnalysis.cpp`` – Debug information extraction from LLVM metadata
* ``ReportOptions.cpp`` – Report configuration options (JSON, SARIF output)

Build Targets
-------------

* ``GVFAChecker`` – GVFA vulnerability checker library
* ``lotus-gvfa`` – Global value flow analysis tool (``tools/checker/lotus_gvfa.cpp``)
* ``lotus-kint`` – KINT numerical bug detection tool (``tools/checker/lotus_kint.cpp``)
* ``lotus-concur`` – Concurrency checker tool (``tools/checker/lotus_concur.cpp``)
* ``lotus-taint`` – Taint analysis tool (``tools/checker/lotus_taint.cpp``)

Usage
-----

**GVFA Tool**:

.. code-block:: bash

   ./build/bin/lotus-gvfa --vuln-type=nullpointer input.bc
   ./build/bin/lotus-gvfa --vuln-type=useafterfree --ctx input.bc
   ./build/bin/lotus-gvfa --vuln-type=nullpointer --use-npa --json-output=report.json input.bc

**KINT Tool**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-all=true input.bc
   ./build/bin/lotus-kint --check-int-overflow=true --check-div-by-zero=true input.bc
   ./build/bin/lotus-kint --report-json=report.json input.bc

**Concurrency Tool**:

.. code-block:: bash

   ./build/bin/lotus-concur --check-data-races input.bc
   ./build/bin/lotus-concur --check-deadlocks --check-atomicity input.bc
   ./build/bin/lotus-concur --report-json=report.json input.bc

Programmatic Usage
------------------

All checkers integrate with the centralized ``BugReportMgr``:

.. code-block:: cpp

   #include "Checker/Report/BugReportMgr.h"
   #include "Checker/GVFA/NullPointerChecker.h"
   
   // Create checker
   auto checker = std::make_unique<NullPointerChecker>();
   
   // Run analysis (automatically reports to BugReportMgr)
   int vulnCount = checker->detectAndReport(M, &GVFA, false, false);
   
   // Access centralized reports
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, 0);

Bug Types
---------

The framework detects the following bug categories:

* **Memory Safety**: Null pointer dereference, use-after-free, uninitialized variables, invalid free, stack address misuse
* **Numerical Errors**: Integer overflow, division by zero, bad shift, array out-of-bounds, dead branches
* **Concurrency**: Data races, deadlocks, atomicity violations

All bug types are classified by importance (LOW, MEDIUM, HIGH) and category (SECURITY, ERROR, WARNING, PERFORMANCE) with CWE mappings.

Integration Points
------------------

* **Global Value Flow Analysis**: Used by GVFA checkers for source-sink reachability
* **Dyck Alias Analysis**: Pointer analysis for GVFA
* **Null Check Analysis**: Optional precision improvement for null pointer checker
* **Z3 SMT Solver**: Used by KINT for path-sensitive verification
* **LLVM Pass Infrastructure**: Standard pass registration for integration

See Also
--------

.. toctree::
   :maxdepth: 1

   concurrency
   gvfa
   kint
