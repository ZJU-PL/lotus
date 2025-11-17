Application Frameworks
====================

Lotus includes several application frameworks for specific analysis tasks, located in ``lib/Apps/``.

Checker Framework
-----------------

General-purpose bug checking utilities and analyzers for detecting various program defects.

**Location**: ``lib/Apps/Checker/``

**Components**:

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

**Build Targets**: 
* ``lotus-gvfa`` – Global value flow analysis tool
* ``lotus-kint`` – KINT numerical bug detection tool
* ``lotus-concur`` – Concurrency checker tool

**Usage**: Integrated into various analysis pipelines for automated bug detection.

CLAM (CLang Abstract Machine)
-----------------------------

Abstract interpretation framework for numerical analysis and property checking.

**Location**: ``lib/Apps/clam/``

**Structure**:

* **Core Components**:
  * ``Clam.cc`` – Main CLAM analysis engine
  * ``CfgBuilder*.cc`` – Control flow graph construction from LLVM IR
  * ``CrabDomainParser.cc`` – Abstract domain configuration parsing
  * ``RegisterAnalysis.cc`` – Analysis pass registration

* **crab/** – CRAB abstract domain library integration
  * **domains/** – Abstract domain implementations
    * ``intervals.cc``, ``wrapped_intervals.cc``, ``terms_intervals.cc`` – Interval domains
    * ``oct.cc``, ``split_oct.cc``, ``terms_zones.cc`` – Octagon domains
    * ``pk.cc``, ``pk_pplite.cc`` – Polyhedra domains
    * ``boxes.cc`` – Box domain
    * ``dis_intervals.cc``, ``terms_dis_intervals.cc`` – Disjunctive intervals
    * ``ric.cc`` – Reduced product of intervals and congruences
    * ``sign_constant.cc`` – Sign and constant domain
    * ``split_dbm.cc`` – Split difference bound matrices
  * **output/** – Output formats
    * **crabir/** – CrabIR format output
    * **json/** – JSON format output
  * **path_analysis/** – Path-sensitive analysis

* **Properties/** – Property checkers
  * ``BndCheck.cc`` – Buffer bounds checking
  * ``NullCheck.cc`` – Null pointer checking
  * ``UafCheck.cc`` – Use-after-free checking

* **Transforms/** – LLVM IR transformations
  * ``DevirtFunctions.cc``, ``DevirtFunctionsPass.cc`` – Function devirtualization
  * ``ExternalizeFunctions.cc``, ``ExternalizeAddressTakenFunctions.cc`` – Function externalization
  * ``LoopPeeler.cc`` – Loop peeling
  * ``LowerCstExpr.cc``, ``LowerSelect.cc``, ``LowerUnsignedICmp.cc`` – Expression lowering
  * ``NondetInit.cc`` – Non-deterministic initialization
  * ``PromoteAssume.cc``, ``PromoteMalloc.cc`` – Promotion passes
  * ``InsertEntryPoint.cc`` – Entry point insertion
  * ``MarkInternalInline.cc`` – Internal function marking
  * ``RemoveUnreachableBlocksPass.cc`` – Unreachable block removal
  * **PropertyInstrumentation/** – Property-specific instrumentation
    * ``MemoryCheck.cc`` – Memory safety checks
    * ``NullCheck.cc`` – Null pointer checks
    * ``UseAfterFreeCheck.cc`` – Use-after-free checks

* **Optimizer/** – Post-analysis optimizations
  * ``Optimizer.cc`` – Analysis result optimization

* **Support/** – Utility functions
  * ``CFGPrinter.cc`` – Control flow graph printing
  * ``BoostException.cc`` – Boost exception handling

**Build Targets**: 
* ``clam`` – Main CLAM analysis tool
* ``clam-pp`` – CLAM preprocessor
* ``clam-diff`` – CLAM diff tool

**Usage**:
.. code-block:: bash

   ./build/bin/clam --crab-domains=intervals example.bc

**Key Options**:
* ``--crab-domains=<domain>``: Analysis domain (intervals, octagons, polyhedra)
* ``--crab-inter``: Interprocedural analysis
* ``--crab-widening-delay=<n>``: Widening delay parameter

Fuzzing Support
--------------

Directed greybox fuzzing techniques and profiling infrastructure for test generation.

**Location**: ``lib/Apps/Fuzzing/``

**Components**:

* **Analysis/** – Distance analysis for directed fuzzing
  * ``BasicBlockDistance.cpp`` – AFLGo basic block distance analysis
  * ``FunctionDistance.cpp`` – Hawkeye function distance analysis
  * ``DAFL.cpp`` – DAFL data-flow distance analysis
  * ``ExtendedCallGraphAnalysis.cpp`` – Enhanced call graph with pointer analysis
  * ``TargetDetection.cpp`` – Target identification for instrumentation

* **AFLGoCompiler/** – AFLGo compiler plugin
  * ``Plugin.cpp`` – LLVM plugin for AFLGo instrumentation
  * ``TargetInjection.cpp`` – Target injection for directed fuzzing

* **AFLGoLinker/** – AFLGo linker plugin
  * ``Plugin.cpp`` – Link-time instrumentation
  * ``DAFL.cpp`` – DAFL-specific instrumentation
  * ``DistanceInstrumentation.cpp`` – Distance-based instrumentation
  * ``FunctionDistanceInstrumentation.cpp`` – Function-level distance instrumentation

* **IRMutate/** – IR mutation utilities
  * ``llvm_mutate.cpp`` – LLVM IR mutation passes

**Algorithms Supported**:
* **AFLGo (CCS 17)**: Distance-based power scheduling, call graph distances
* **Hawkeye (CCS 18)**: Function-level analysis, rare branch prioritization
* **DAFL (USENIX Security 23)**: Data dependency guided, taint integration

**Usage**: Instrumentation for coverage-guided and directed fuzzing workflows.

MCP (Model Checking Platform)
-----------------------------

Model checking framework integration and server implementation.

**Location**: ``lib/Apps/MCP/``

**Components**:
* ``MCPServer.cpp`` – Model checking platform server implementation

**Build Target**: ``tools/mcp``

**Features**: Model checking algorithm implementations and verification utilities.

SeaHorn Verification Framework
------------------------------

Large-scale verification framework with SMT-based model checking and Horn clause solving.

**Location**: ``lib/Apps/seahorn/``

**Structure**:

* **seahorn/** – Core SeaHorn verification engine
  * Horn clause generation and solving (HornifyFunction, HornifyModule)
  * Symbolic execution engines (BvOpSem, ClpOpSem, UfoOpSem)
  * Bounded model checking (Bmc, PathBmc)
  * Counterexample generation (CexHarness, CexExeGenerator)
  * **Smt/** – SMT solver integration (Z3, Yices2)

* **Analysis/** – Analysis passes for verification
  * ``CrabAnalysis.cc`` – CRAB abstract interpretation integration
  * ``CanAccessMemory.cc`` – Memory access analysis
  * ``CanFail.cc`` – Failure point analysis
  * ``CanReadUndef.cc`` – Undefined value read analysis
  * ``ClassHierarchyAnalysis.cc`` – C++ class hierarchy analysis
  * ``ControlDependenceAnalysis.cc`` – Control dependence computation
  * ``CutPointGraph.cc`` – Cut point graph construction
  * ``GateAnalysis.cc`` – Gate analysis for verification
  * ``StaticTaint.cc`` – Static taint analysis
  * ``TopologicalOrder.cc``, ``WeakTopologicalOrder.cc`` – Ordering analyses

* **Transforms/** – LLVM IR transformations for verification
  * **Instrumentation/** – Property instrumentation
    * ``BufferBoundsCheck.cc``, ``FatBufferBoundsCheck.cc`` – Buffer bounds checking
    * ``NullCheck.cc`` – Null pointer checking
    * ``SimpleMemoryCheck.cc`` – Memory safety checks
    * ``MixedSemantics.cc`` – Mixed operational semantics
  * **Scalar/** – Scalar optimizations
    * ``CutLoops.cc`` – Loop cutting
    * ``LoopPeeler.cc`` – Loop peeling
    * ``LowerCstExpr.cc`` – Constant expression lowering
    * ``LowerGvInitializers.cc`` – Global variable initializer lowering
    * ``PromoteVerifierCalls.cc`` – Verifier call promotion
    * ``BackedgeCutter.cc`` – Backedge cutting
  * **Utils/** – Transformation utilities
    * ``DevirtFunctions.cc`` – Function devirtualization
    * ``ExternalizeFunctions.cc`` – Function externalization
    * ``Mem2Reg.cc`` – Memory-to-register promotion
    * ``SliceFunctions.cc`` – Function slicing

* **Support/** – Utility functions
  * ``CFGPrinter.cc`` – Control flow graph printing
  * ``GitSHA1.cc`` – Version information
  * ``Profiler.cc`` – Profiling support
  * ``Stats.cc`` – Statistics collection

**Build Targets**: 
* ``tools/seahorn`` – Main SeaHorn verification tool
* ``tools/verifier/seahorn/seahorn`` – SeaHorn verification frontend
* ``tools/verifier/seahorn/seapp`` – SeaHorn preprocessing tool
* ``tools/verifier/seahorn/seainspect`` – SeaHorn inspection tool

**Features**:
* **Horn clause solving** – CHC-based verification
* **Abstract interpretation** – Numerical and shape analysis integration
* **Counterexample generation** – Error trace production
* **Modular verification** – Compositional reasoning
* **Bounded model checking** – BMC with configurable bounds

For a detailed usage and workflow description, see :doc:`seahorn`.
