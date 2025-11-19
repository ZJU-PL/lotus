GVFA Vulnerability Checkers
============================

Global Value Flow Analysis (GVFA) based checkers for detecting memory safety vulnerabilities in LLVM bitcode.

**Library Location**: ``lib/Checker/GVFA/``

**Headers**: ``include/Checker/GVFA/``

**Tool Location**: ``tools/checker/lotus_gvfa.cpp``

**Build Target**: ``lotus-gvfa``

Overview
--------

GVFA checkers use Global Value Flow Analysis to detect memory safety vulnerabilities by tracking value flows from sources (where vulnerabilities originate) to sinks (where vulnerabilities manifest). The analysis uses Dyck alias analysis to track pointer relationships and value flow through the program.

All checkers implement the ``GVFAVulnerabilityChecker`` interface and report bugs through the centralized ``BugReportMgr`` system.

Components
----------

**GVFAVulnerabilityChecker** (``GVFAVulnerabilityChecker.h``):

* Base interface for all GVFA vulnerability checkers
* Defines source/sink identification and reachability checking
* Provides unified API: ``getSources()``, ``getSinks()``, ``detectAndReport()``

**NullPointerChecker** (``NullPointerChecker.cpp``, ``NullPointerChecker.h``):

* Detects null pointer dereferences (CWE-476, CWE-690)
* **Sources**: NULL constants, memory allocation functions (can return NULL)
* **Sinks**: Pointer dereferences, library functions that dereference arguments
* **Optional Precision**: Can use ``NullCheckAnalysis`` or ``ContextSensitiveNullCheckAnalysis`` for improved precision

**UseAfterFreeChecker** (``UseAfterFreeChecker.cpp``, ``UseAfterFreeChecker.h``):

* Detects use-after-free vulnerabilities (CWE-416)
* **Sources**: Memory deallocation operations (free, delete, etc.)
* **Sinks**: Memory accesses (loads, stores, function calls with pointer arguments)

**UseOfUninitializedVariableChecker** (``UseOfUninitializedVariableChecker.cpp``, ``UseOfUninitializedVariableChecker.h``):

* Detects use of uninitialized variables (CWE-457)
* **Sources**: Uninitialized memory locations
* **Sinks**: Uses of variables before initialization

**FreeOfNonHeapMemoryChecker** (``FreeOfNonHeapMemoryChecker.cpp``, ``FreeOfNonHeapMemoryChecker.h``):

* Detects invalid free() operations on non-heap memory (CWE-590)
* **Sources**: Stack addresses, global variables, string literals
* **Sinks**: Memory deallocation operations (free, delete)

**InvalidUseOfStackAddressChecker** (``InvalidUseOfStackAddressChecker.cpp``, ``InvalidUseOfStackAddressChecker.h``):

* Detects invalid use of stack addresses (CWE-562)
* **Sources**: Stack-allocated variables
* **Sinks**: Operations that require heap memory (return statements, global storage, etc.)

Usage
-----

**Basic Usage**:

.. code-block:: bash

   ./build/bin/lotus-gvfa --vuln-type=nullpointer input.bc
   ./build/bin/lotus-gvfa --vuln-type=useafterfree input.bc
   ./build/bin/lotus-gvfa --vuln-type=uninitialized input.bc
   ./build/bin/lotus-gvfa --vuln-type=freenonheap input.bc
   ./build/bin/lotus-gvfa --vuln-type=stackaddress input.bc

**With Context-Sensitive Analysis**:

.. code-block:: bash

   ./build/bin/lotus-gvfa --vuln-type=nullpointer --ctx input.bc

**With Null Check Analysis (Improved Precision)**:

.. code-block:: bash

   ./build/bin/lotus-gvfa --vuln-type=nullpointer --use-npa input.bc
   ./build/bin/lotus-gvfa --vuln-type=nullpointer --use-npa --ctx input.bc

**Generate JSON Report**:

.. code-block:: bash

   ./build/bin/lotus-gvfa --vuln-type=nullpointer --json-output=report.json input.bc

**Verbose Output**:

.. code-block:: bash

   ./build/bin/lotus-gvfa --vuln-type=nullpointer --verbose input.bc

**Dump Statistics**:

.. code-block:: bash

   ./build/bin/lotus-gvfa --vuln-type=nullpointer --dump-stats input.bc

Command-Line Options
--------------------

* ``--vuln-type=<type>`` – Vulnerability type to check:
  
  * ``nullpointer`` – Null pointer dereference
  * ``useafterfree`` – Use after free
  * ``uninitialized`` – Use of uninitialized variable
  * ``freenonheap`` – Free of non-heap memory
  * ``stackaddress`` – Invalid use of stack address

* ``--ctx`` – Use context-sensitive analysis (default: false)
* ``--use-npa`` – Use NullCheckAnalysis to improve precision (nullpointer only, default: false)
* ``--verbose`` – Print detailed vulnerability information (default: false)
* ``--dump-stats`` – Dump analysis statistics (default: false)
* ``--json-output=<file>`` – Output JSON report to file
* ``--min-score=<n>`` – Minimum confidence score for reporting (0-100)

Bug Types
---------

**Null Pointer Dereference** (``BUG_NPD``):

* **CWE**: CWE-476, CWE-690
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Dereferencing a pointer that may be NULL

**Use After Free** (``BUG_UAF``):

* **CWE**: CWE-416
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Using memory after it has been freed

**Use of Uninitialized Variable** (``BUG_UUV``):

* **CWE**: CWE-457
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Using a variable before it has been initialized

**Free of Non-Heap Memory** (``BUG_FNHM``):

* **CWE**: CWE-590
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Calling free() on memory that was not allocated on the heap

**Invalid Use of Stack Address** (``BUG_IUSA``):

* **CWE**: CWE-562
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Using a stack address in a context that requires heap memory

Analysis Process
----------------

1. **Setup GVFA**: Initialize Dyck alias analysis and value flow graph
2. **Identify Sources**: Find vulnerability sources (e.g., NULL constants, free operations)
3. **Identify Sinks**: Find vulnerability sinks (e.g., pointer dereferences, memory accesses)
4. **Check Reachability**: Use GVFA to check if values can flow from sources to sinks
5. **Report Vulnerabilities**: Generate bug reports through ``BugReportMgr``

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Checker/GVFA/NullPointerChecker.h"
   #include "Checker/Report/BugReportMgr.h"
   #include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
   
   // Setup GVFA (requires Dyck alias analysis)
   DyckAliasAnalysis *DyckAA = new DyckAliasAnalysis();
   DyckModRefAnalysis *DyckMRA = new DyckModRefAnalysis();
   DyckVFG VFG(DyckAA, DyckMRA, M);
   DyckGlobalValueFlowAnalysis GVFA(M, &VFG, DyckAA, DyckMRA);
   
   // Create checker
   auto checker = std::make_unique<NullPointerChecker>();
   
   // Optional: Improve precision with NullCheckAnalysis
   NullCheckAnalysis *NCA = new NullCheckAnalysis();
   checker->setNullCheckAnalysis(NCA);
   
   // Run analysis (automatically reports to BugReportMgr)
   int vulnCount = checker->detectAndReport(M, &GVFA, false, false);
   
   // Access centralized reports
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, 0);

Creating Custom Checkers
-------------------------

To create a new GVFA checker, inherit from ``GVFAVulnerabilityChecker``:

.. code-block:: cpp

   #include "Checker/GVFA/GVFAVulnerabilityChecker.h"
   
   class MyCustomChecker : public GVFAVulnerabilityChecker {
   public:
       void getSources(Module *M, VulnerabilitySourcesType &Sources) override {
           // Identify vulnerability sources
       }
       
       void getSinks(Module *M, VulnerabilitySinksType &Sinks) override {
           // Identify vulnerability sinks
       }
       
       bool isValidTransfer(const Value *From, const Value *To) const override {
           // Check if value transfer is valid for this vulnerability type
           return true;
       }
       
       std::string getCategory() const override {
           return "MyCustomVulnerability";
       }
       
       int registerBugType() override {
           BugReportMgr& mgr = BugReportMgr::get_instance();
           return mgr.register_bug_type("My Custom Bug", 
                                        BugDescription::BI_HIGH,
                                        BugDescription::BC_SECURITY,
                                        "CWE-XXX");
       }
       
       void reportVulnerability(int bugTypeId, const Value *Source, 
                                const Value *Sink, 
                                const std::set<const Value *> *SinkInsts) override {
           // Create and report bug
       }
   };

Statistics
----------

The GVFA analysis provides statistics:

* ``AllQueryCounter`` – Total number of reachability queries
* ``SuccsQueryCounter`` – Number of successful queries (vulnerabilities found)
* Query success rate and timing information

Limitations
-----------

* **Conservative Analysis**: May report false positives due to over-approximation in alias analysis
* **Context Sensitivity**: Context-sensitive analysis is more precise but slower
* **Limited Library Function Modeling**: Some library functions may not be fully modeled
* **No Runtime Information**: Static analysis only, cannot detect runtime-specific issues

Performance
-----------

* GVFA scales with program size and pointer complexity
* Context-sensitive analysis is more precise but significantly slower
* NullCheckAnalysis improves precision for null pointer checker with moderate overhead
* Statistics can help identify performance bottlenecks

Integration
-----------

GVFA checkers integrate with:

* **Dyck Alias Analysis**: Pointer analysis for value flow tracking
* **Global Value Flow Analysis**: Core value flow analysis engine
* **Null Check Analysis**: Optional precision improvement for null pointer checker
* **BugReportMgr**: Centralized bug reporting system

See Also
--------

- :doc:`index` – Checker Framework overview
- :doc:`../analysis/gvfa` – Global Value Flow Analysis documentation
- :doc:`../alias/index` – Alias analysis for pointer information

