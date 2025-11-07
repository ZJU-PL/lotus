Concurrency Bug Checker
========================

Static analysis tool for detecting concurrency bugs: data races, deadlocks, and atomicity violations.

Overview
--------

Analyzes LLVM IR to detect concurrency issues in multithreaded programs.

**Location**: ``tools/checker/lotus_concur.cpp``

**Features**: Data race detection, deadlock detection, atomicity violations, happens-before analysis.

Usage
-----

.. code-block:: bash

   ./build/bin/lotus-concur [options] <input.bc>
   ./build/bin/lotus-concur --report-json=report.json input.bc

**Options**: ``--check-data-races``, ``--check-deadlocks``, ``--check-atomicity``, ``--report-json=<file>``

Bug Types
---------

**Data Races**: Concurrent access to shared memory without synchronization.

**Deadlocks**: Circular lock dependencies between threads.

**Atomicity Violations**: Non-atomic sequences of operations.

Analysis Process
----------------

1. Parse LLVM IR with concurrency primitives
2. Build happens-before relationships
3. Perform lockset analysis
4. Detect bug patterns
5. Generate reports

Performance
-----------

Handles multiple threads efficiently, conservative analysis may have false positives.


Limitations
-----------

Conservative analysis, limited custom synchronization support.
