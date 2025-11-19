Concurrency Bug Checker
========================

Static analysis tool for detecting concurrency bugs in multithreaded programs: data races, deadlocks, and atomicity violations.

**Library Location**: ``lib/Checker/Concurrency/``

**Headers**: ``include/Checker/Concurrency/``

**Tool Location**: ``tools/checker/lotus_concur.cpp``

**Build Target**: ``lotus-concur``

Overview
--------

The concurrency checker analyzes LLVM IR to detect thread safety issues using:

* **MHP (May Happen in Parallel) Analysis** – Determines which instructions can execute concurrently
* **Lock Set Analysis** – Tracks which locks protect shared memory accesses
* **Happens-Before Analysis** – Builds synchronization relationships between threads

All detected bugs are reported through the centralized ``BugReportMgr`` system, enabling unified JSON and SARIF output.

Components
----------

**ConcurrencyChecker** (``ConcurrencyChecker.cpp``, ``ConcurrencyChecker.h``):

* Main coordinator that orchestrates all concurrency checks
* Manages statistics and analysis configuration
* Integrates with ``BugReportMgr`` for centralized reporting

**DataRaceChecker** (``DataRaceChecker.cpp``, ``DataRaceChecker.h``):

* Detects data races using MHP analysis
* Identifies concurrent memory accesses without proper synchronization
* Reports bug type: ``BUG_DATARACE`` (CWE-362, HIGH importance, ERROR classification)

**DeadlockChecker** (``DeadlockChecker.cpp``, ``DeadlockChecker.h``):

* Detects potential deadlocks using lock set analysis
* Identifies circular lock dependencies between threads
* Analyzes lock acquisition order to find cycles

**AtomicityChecker** (``AtomicityChecker.cpp``, ``AtomicityChecker.h``):

* Detects atomicity violations
* Identifies non-atomic sequences of operations that should be atomic
* Checks for interleaving of critical sections

Usage
-----

**Basic Usage**:

.. code-block:: bash

   ./build/bin/lotus-concur input.bc

**Enable Specific Checks**:

.. code-block:: bash

   ./build/bin/lotus-concur --check-data-races input.bc
   ./build/bin/lotus-concur --check-deadlocks input.bc
   ./build/bin/lotus-concur --check-atomicity input.bc

**Enable Multiple Checks**:

.. code-block:: bash

   ./build/bin/lotus-concur --check-data-races --check-deadlocks --check-atomicity input.bc

**Generate JSON Report**:

.. code-block:: bash

   ./build/bin/lotus-concur --report-json=report.json input.bc

**Generate SARIF Report**:

.. code-block:: bash

   ./build/bin/lotus-concur --report-sarif=report.sarif input.bc

Command-Line Options
--------------------

* ``--check-data-races`` – Enable data race detection (default: true)
* ``--check-deadlocks`` – Enable deadlock detection (default: true)
* ``--check-atomicity`` – Enable atomicity violation detection (default: true)
* ``--report-json=<file>`` – Output JSON report to file
* ``--report-sarif=<file>`` – Output SARIF report to file
* ``--min-score=<n>`` – Minimum confidence score for reporting (0-100)

Bug Types
---------

**Data Races** (``BUG_DATARACE``):

* **Description**: Concurrent access to shared memory without proper synchronization
* **CWE**: CWE-362
* **Importance**: HIGH
* **Classification**: ERROR
* **Detection**: MHP analysis identifies instructions that may execute in parallel and access the same memory location without locks

**Deadlocks**:

* **Description**: Circular lock dependencies between threads that can cause threads to wait indefinitely
* **Detection**: Lock set analysis identifies cycles in lock acquisition order
* **Analysis**: Tracks lock acquisition sequences across threads

**Atomicity Violations**:

* **Description**: Non-atomic sequences of operations that should be atomic
* **Detection**: Identifies critical sections that can be interleaved incorrectly
* **Analysis**: Checks for proper synchronization around related operations

Analysis Process
----------------

1. **Parse LLVM IR**: Load module and identify concurrency primitives (threads, locks, synchronization)
2. **Build MHP Graph**: Determine which instructions may happen in parallel
3. **Lock Set Analysis**: Track which locks protect each memory access
4. **Happens-Before Analysis**: Build synchronization relationships (mutexes, barriers, etc.)
5. **Pattern Detection**: Identify data races, deadlocks, and atomicity violations
6. **Report Generation**: Generate reports through centralized ``BugReportMgr``

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Checker/Concurrency/ConcurrencyChecker.h"
   #include "Checker/Report/BugReportMgr.h"
   
   using namespace concurrency;
   
   // Create checker
   ConcurrencyChecker checker(*module);
   
   // Configure checks
   checker.enableDataRaceCheck(true);
   checker.enableDeadlockCheck(true);
   checker.enableAtomicityCheck(true);
   
   // Run analysis (automatically reports to BugReportMgr)
   checker.runChecks();
   
   // Get statistics
   auto stats = checker.getStatistics();
   outs() << "Data Races Found: " << stats.dataRacesFound << "\n";
   outs() << "Deadlocks Found: " << stats.deadlocksFound << "\n";
   outs() << "Atomicity Violations Found: " << stats.atomicityViolationsFound << "\n";
   
   // Access centralized reports
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());

Statistics
----------

The checker provides detailed analysis statistics:

* ``totalInstructions`` – Total number of instructions analyzed
* ``mhpPairs`` – Number of instruction pairs that may happen in parallel
* ``locksAnalyzed`` – Number of lock operations analyzed
* ``dataRacesFound`` – Number of data races detected
* ``deadlocksFound`` – Number of deadlocks detected
* ``atomicityViolationsFound`` – Number of atomicity violations detected

Limitations
-----------

* **Conservative Analysis**: May report false positives due to over-approximation
* **Limited Synchronization Support**: Primarily supports standard pthread and C++11 synchronization primitives
* **Context-Insensitive**: Does not distinguish between different call contexts
* **No Dynamic Analysis**: Static analysis only, cannot detect runtime-specific issues

Performance
-----------

* Handles multiple threads efficiently using graph-based algorithms
* MHP analysis scales with the number of threads and instructions
* Lock set analysis is efficient for typical lock usage patterns
* Conservative analysis may have false positives but ensures soundness

Integration
-----------

The concurrency checker integrates with:

* **BugReportMgr**: Centralized bug reporting system
* **LLVM IR**: Standard LLVM intermediate representation
* **Dyck Alias Analysis**: Pointer analysis for memory access tracking

See Also
--------

- :doc:`index` – Checker Framework overview
- :doc:`../alias/index` – Alias analysis for pointer information
- :doc:`../analysis/index` – Analysis infrastructure
