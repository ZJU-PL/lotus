KINT Numerical Bug Checker
===========================

Static analysis tool for detecting numerical bugs: integer overflow, division by zero, array bounds violations, and related issues.

**Library Location**: ``lib/Checker/KINT/``

**Headers**: ``include/Checker/KINT/``

**Tool Location**: ``tools/checker/lotus_kint.cpp``

**Build Target**: ``lotus-kint``

Overview
--------

KINT (Kint Is Not Taint) uses range analysis and SMT solving to detect numerical bugs in LLVM bitcode. It combines:

* **Range Analysis**: Abstract interpretation to compute value ranges for variables
* **SMT Solving**: Z3-based path-sensitive verification for precise bug detection
* **Taint Analysis**: Tracking of untrusted data sources

All detected bugs are reported through the centralized ``BugReportMgr`` system, enabling unified JSON and SARIF output.

Components
----------

**MKintPass** (``MKintPass.cpp``, ``MKintPass.h``):

* Main LLVM pass that orchestrates all KINT analyses
* Integrates range analysis, taint analysis, and bug detection
* Reports bugs through ``BugReportMgr``

**RangeAnalysis** (``RangeAnalysis.cpp``, ``RangeAnalysis.h``):

* Computes value ranges for variables using abstract interpretation
* Performs intraprocedural and interprocedural range propagation
* Handles loops, branches, and function calls

**KINTTaintAnalysis** (``KINTTaintAnalysis.cpp``, ``KINTTaintAnalysis.h``):

* Tracks taint sources (untrusted inputs)
* Propagates taint through the program
* Identifies tainted values used in security-critical operations

**BugDetection** (``BugDetection.cpp``, ``BugDetection.h``):

* Detects specific bug patterns:
  * Integer overflow
  * Division by zero
  * Bad shift operations
  * Array out-of-bounds access
  * Dead branches (unreachable code)

**Options** (``Options.cpp``, ``Options.h``):

* Command-line option parsing and configuration
* Checker enable/disable flags
* Performance tuning options

**Log** (``Log.cpp``, ``Log.h``):

* Logging infrastructure with configurable levels
* Supports file and stderr output

**Utils** (``Utils.cpp``, ``Utils.h``):

* Utility functions for analysis

Usage
-----

**Enable All Checkers**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-all=true input.bc

**Enable Specific Checkers**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-int-overflow=true input.bc
   ./build/bin/lotus-kint --check-div-by-zero=true input.bc
   ./build/bin/lotus-kint --check-bad-shift=true input.bc
   ./build/bin/lotus-kint --check-array-oob=true input.bc
   ./build/bin/lotus-kint --check-dead-branch=true input.bc

**Enable Multiple Checkers**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-int-overflow=true --check-div-by-zero=true input.bc

**Generate JSON Report**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-all=true --report-json=report.json input.bc

**Generate SARIF Report**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-all=true --report-sarif=report.sarif input.bc

**Verbose Logging**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-all=true --log-level=debug input.bc

**Function Timeout**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-all=true --function-timeout=60 input.bc

Command-Line Options
--------------------

**Checker Options**:

* ``--check-all=<true|false>`` – Enable all checkers at once (default: false)
* ``--check-int-overflow=<true|false>`` – Enable integer overflow detection (default: false)
* ``--check-div-by-zero=<true|false>`` – Enable division by zero detection (default: false)
* ``--check-bad-shift=<true|false>`` – Enable bad shift detection (default: false)
* ``--check-array-oob=<true|false>`` – Enable array out-of-bounds detection (default: false)
* ``--check-dead-branch=<true|false>`` – Enable dead branch detection (default: false)

**Performance Options**:

* ``--function-timeout=<seconds>`` – Timeout per function for SMT solving (0 = no limit, default: varies)

**Logging Options**:

* ``--log-level=<debug|info|warning|error|none>`` – Set logging level (default: info)
* ``--quiet`` – Suppress all output (default: false)
* ``--stderr-logging`` – Log to stderr instead of stdout (default: false)
* ``--log-file=<file>`` – Log to file (default: stdout/stderr)

**Report Options**:

* ``--report-json=<file>`` – Output JSON report to file
* ``--report-sarif=<file>`` – Output SARIF report to file
* ``--min-score=<n>`` – Minimum confidence score for reporting (0-100)

Bug Types
---------

**Integer Overflow** (``Integer Overflow``):

* **CWE**: CWE-190
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Arithmetic operations that may overflow integer bounds

**Divide by Zero** (``Divide by Zero``):

* **CWE**: CWE-369
* **Importance**: MEDIUM
* **Classification**: ERROR
* **Description**: Division operations where the divisor may be zero

**Bad Shift** (``Bad Shift``):

* **Importance**: MEDIUM
* **Classification**: ERROR
* **Description**: Shift operations with invalid shift amounts

**Array Out of Bounds** (``Array Out of Bounds``):

* **CWE**: CWE-119, CWE-125
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Array accesses that may be outside array bounds

**Dead Branch** (``Dead Branch``):

* **Importance**: LOW
* **Classification**: ERROR
* **Description**: Unreachable code branches

Analysis Process
----------------

1. **Range Initialization**: Initialize value ranges for global variables and function parameters
2. **Taint Source Identification**: Mark taint sources (untrusted inputs)
3. **Range Propagation**: Propagate ranges through the program using abstract interpretation
4. **Backedge Analysis**: Identify loop backedges for range widening
5. **SMT Solving**: Use Z3 to verify bug conditions path-sensitively
6. **Bug Detection**: Identify specific bug patterns based on ranges and SMT results
7. **Report Generation**: Generate reports through centralized ``BugReportMgr``

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Checker/KINT/MKintPass.h"
   #include "Checker/Report/BugReportMgr.h"
   
   using namespace kint;
   
   // Create and run the pass
   llvm::ModuleAnalysisManager MAM;
   llvm::ModulePassManager MPM;
   llvm::PassBuilder PB;
   
   PB.registerModuleAnalyses(MAM);
   MPM.addPass(kint::MKintPass());
   
   // Run analysis (automatically reports to BugReportMgr)
   MPM.run(*M, MAM);
   
   // Access centralized reports
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, 0);

Range Analysis
--------------

KINT uses abstract interpretation to compute value ranges:

* **Interval Domain**: Tracks lower and upper bounds for integer values
* **Widening**: Handles loops using widening operators
* **Narrowing**: Improves precision after widening
* **Function Summaries**: Interprocedural range propagation

SMT Solving
-----------

For path-sensitive verification, KINT uses Z3:

* **Path Constraints**: Builds constraints along execution paths
* **Symbolic Execution**: Creates symbolic expressions for variables
* **Satisfiability Checking**: Verifies if bug conditions are satisfiable
* **Timeout Handling**: Limits analysis time per function

Taint Analysis
--------------

KINT tracks taint sources and propagation:

* **Taint Sources**: Functions that read untrusted input (e.g., ``read()``, ``recv()``)
* **Taint Propagation**: Tracks how tainted values flow through the program
* **Taint Sinks**: Security-critical operations that use tainted data

Limitations
-----------

* **SMT Solver Timeout**: Complex functions may timeout, leading to incomplete analysis
* **Range Precision**: Abstract interpretation may over-approximate, causing false positives
* **Loop Handling**: Complex loops may require manual widening hints
* **Floating Point**: Limited support for floating-point operations
* **Context Sensitivity**: Intraprocedural analysis may miss interprocedural bugs

Performance
-----------

* Range analysis is fast and scales well with program size
* SMT solving can be slow for complex functions (use timeouts)
* Function timeout helps prevent analysis from getting stuck
* Statistics can help identify performance bottlenecks

Integration
-----------

KINT integrates with:

* **Z3 SMT Solver**: Path-sensitive verification
* **LLVM Pass Infrastructure**: Standard pass registration
* **BugReportMgr**: Centralized bug reporting system
* **Taint Analysis**: Security-focused taint tracking

See Also
--------

- :doc:`index` – Checker Framework overview
- :doc:`../solvers/index` – SMT solver integration
- :doc:`../analysis/index` – Analysis infrastructure

