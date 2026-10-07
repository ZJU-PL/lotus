Developer Guide
===============

This guide explains how to extend Lotus with custom analyses, checkers, and tools.

Development Environment Setup
------------------------------

Prerequisites
~~~~~~~~~~~~~

- LLVM 14.0.0 development libraries
- Z3 4.11 with headers
- CMake 3.18+
- C++17 compatible compiler (GCC 7+, Clang 5+)
- Git for version control

Building from Source
~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   git clone https://github.com/ZJU-PL/lotus
   cd lotus
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Debug
   make -j$(nproc)

Debug build enables assertions and debugging symbols.

IDE Configuration
~~~~~~~~~~~~~~~~~

**VSCode**: Create ``.vscode/c_cpp_properties.json``:

.. code-block:: json

   {
       "configurations": [{
           "name": "Linux",
           "includePath": [
               "${workspaceFolder}/include",
               "${workspaceFolder}/build/include",
               "/path/to/llvm/include",
               "/usr/include/z3"
           ],
           "compileCommands": "${workspaceFolder}/build/compile_commands.json"
       }]
   }

**CLion**: Open the project and point to ``build/compile_commands.json``.

Code Organization
-----------------

Directory Structure
~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   lotus/
   ├── include/           # Public headers (mirrors lib structure)
   │   ├── Alias/         # Alias analysis (DyckAA, AserPTA, LotusAA, SparrowAA, etc.)
   │   ├── Analysis/      # Analysis utilities (NullPointer, CFG, etc.)
   │   ├── CFL/           # CFL reachability
   │   ├── Checker/       # Bug checkers (AE, Concurrency, FiTx, KINT, Pulse, Saber, etc.)
   │   ├── Concurrency/   # Concurrency analyses (MHP, lockset, OpenMP, CUDA, etc.)
   │   ├── Dataflow/      # APA, IFDS/IDE, Mono, NPA, VASCO, WPDS
   │   ├── IR/            # GSA, GVFG, ICFG, PDG, SSI, SVFG, vSSA, etc.
   │   ├── Solvers/       # Datalog, EGraph, SMT
   │   ├── SymbolicExecution/ # Symbolic execution
   │   ├── Transform/     # LLVM bitcode transformations
   │   ├── Utils/         # LLVM utilities, ThreadPool, formats, etc.
   │   └── Verification/  # SIFA, CLAM, smarck, Seahorn, etc.
   ├── lib/               # Implementations (mirrors include)
   ├── tools/             # Command-line tools (alias, checker, verifier, ir, cfl, etc.)
   ├── tests/             # GTest-based tests (tests/unit/ mirrors subsystems)
   ├── benchmarks/        # Benchmark programs
   ├── third-party/       # CUDD, WPDS, spdlog, crab
   ├── scripts/           # Python utilities
   └── docs/              # Sphinx documentation (source/)

Coding Standards
~~~~~~~~~~~~~~~~

**Naming Conventions**:

- Classes: ``CamelCase`` (e.g., ``PointerAnalysis``)
- Functions: ``camelCase`` (e.g., ``getPointsToSet``)
- Variables: ``snake_case`` (e.g., ``points_to_set``)
- Constants: ``UPPER_CASE`` (e.g., ``MAX_ITERATIONS``)
- Member variables: prefix with ``m_`` or ``_``

**Code Style**:

- 2-space indentation
- 100 character line limit
- Use LLVM coding standards where applicable
- Add Doxygen comments for public APIs

Adding a New Alias Analysis
----------------------------

Step 1: Create Directory Structure
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   cd lotus
   mkdir -p include/Alias/MyAA
   mkdir -p lib/Alias/MyAA

Step 2: Define Header
~~~~~~~~~~~~~~~~~~~~~~

Create ``include/Alias/MyAA/MyAliasAnalysis.h``:

.. code-block:: cpp

   #pragma once

   #include "llvm/Pass.h"
   #include "llvm/IR/Module.h"
   #include "llvm/IR/Value.h"
   #include <map>
   #include <set>

   namespace myaa {

   class MyAliasAnalysis : public llvm::ModulePass {
   public:
       static char ID;

       MyAliasAnalysis() : ModulePass(ID) {}

       bool runOnModule(llvm::Module &M) override;

       bool mayAlias(const llvm::Value *V1, const llvm::Value *V2) const;

       void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

       llvm::StringRef getPassName() const override {
           return "My Alias Analysis";
       }

   private:
       std::map<const llvm::Value*, std::set<const llvm::Value*>> points_to_;
       void computePointsTo(llvm::Module &M);
       void processInstruction(llvm::Instruction *I);
   };

   } // namespace myaa

Step 3: Implement Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Alias/MyAA/MyAliasAnalysis.cpp``:

.. code-block:: cpp

   #include "Alias/MyAA/MyAliasAnalysis.h"
   #include "llvm/IR/Instructions.h"
   #include "llvm/Support/raw_ostream.h"

   using namespace llvm;
   using namespace myaa;

   char MyAliasAnalysis::ID = 0;

   bool MyAliasAnalysis::runOnModule(Module &M) {
       points_to_.clear();
       computePointsTo(M);
       return false;
   }

   void MyAliasAnalysis::computePointsTo(Module &M) {
       for (auto &F : M) {
           if (F.isDeclaration()) continue;

           for (auto &BB : F) {
               for (auto &I : BB) {
                   processInstruction(&I);
               }
           }
       }
   }

   void MyAliasAnalysis::processInstruction(Instruction *I) {
       if (auto *alloca = dyn_cast<AllocaInst>(I)) {
           points_to_[alloca].insert(alloca);
       } else if (auto *load = dyn_cast<LoadInst>(I)) {
           Value *ptr = load->getPointerOperand();
           if (points_to_.count(ptr)) {
               for (auto *obj : points_to_[ptr]) {
                   points_to_[load].insert(obj);
               }
           }
       } else if (auto *gep = dyn_cast<GetElementPtrInst>(I)) {
           Value *base = gep->getPointerOperand();
           if (points_to_.count(base)) {
               points_to_[gep] = points_to_[base];
           }
       }
   }

   bool MyAliasAnalysis::mayAlias(const Value *V1, const Value *V2) const {
       if (!points_to_.count(V1) || !points_to_.count(V2)) {
           return true; // Conservative
       }

       const auto &pts1 = points_to_.at(V1);
       const auto &pts2 = points_to_.at(V2);

       for (auto *obj : pts1) {
           if (pts2.count(obj)) {
               return true;
           }
       }
       return false;
   }

   void MyAliasAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
       AU.setPreservesAll();
   }

   static RegisterPass<MyAliasAnalysis> X("my-aa", "My Alias Analysis");

Step 4: Add CMakeLists.txt
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Alias/MyAA/CMakeLists.txt``:

.. code-block:: cmake

   set(SOURCES
       MyAliasAnalysis.cpp
   )

   add_library(CanaryMyAA STATIC ${SOURCES})

   target_include_directories(CanaryMyAA PUBLIC
       ${CMAKE_SOURCE_DIR}/include
       ${LLVM_INCLUDE_DIRS}
   )

   target_link_libraries(CanaryMyAA
       LLVMCore
       LLVMSupport
       LLVMAnalysis
   )

Update ``lib/Alias/CMakeLists.txt``:

.. code-block:: cmake

   add_subdirectory(MyAA)

Step 5: Create Command-Line Tool
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``tools/alias/my-aa.cpp``:

.. code-block:: cpp

   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "llvm/IRReader/IRReader.h"
   #include "llvm/Support/SourceMgr.h"
   #include "llvm/IR/LegacyPassManager.h"
   #include "llvm/Support/CommandLine.h"
   #include "Alias/MyAA/MyAliasAnalysis.h"

   using namespace llvm;

   static cl::opt<std::string> InputFilename(
       cl::Positional, cl::desc("<input bitcode>"), cl::Required);

   int main(int argc, char **argv) {
       cl::ParseCommandLineOptions(argc, argv, "My Alias Analysis Tool\n");

       LLVMContext context;
       SMDiagnostic error;

       std::unique_ptr<Module> module = parseIRFile(InputFilename, error, context);
       if (!module) {
           error.print(argv[0], errs());
           return 1;
       }

       legacy::PassManager PM;
       PM.add(new myaa::MyAliasAnalysis());
       PM.run(*module);

       errs() << "Analysis completed successfully\n";
       return 0;
   }

Update ``tools/alias/CMakeLists.txt``:

.. code-block:: cmake

   add_executable(my-aa my-aa.cpp)

   target_link_libraries(my-aa
       CanaryMyAA
       ${LLVM_LIBS}
   )

Adding a New Bug Checker
-------------------------

Step 1: Define Checker Interface
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``include/Checker/MyChecker.h``:

.. code-block:: cpp

   #pragma once

   #include "llvm/Pass.h"
   #include "llvm/IR/Module.h"
   #include "Checker/Framework/BugReport.h"
   #include "Checker/Framework/BugReportMgr.h"

   namespace lotus {

   class MyChecker : public llvm::ModulePass {
   public:
       static char ID;

       MyChecker() : ModulePass(ID) {}

       bool runOnModule(llvm::Module &M) override;

       llvm::StringRef getPassName() const override {
           return "My Bug Checker";
       }

   private:
       int bugTypeId_ = -1;
       void checkFunction(llvm::Function &F);
   };

   } // namespace lotus

Step 2: Implement Checker
~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Checker/MyChecker.cpp``:

.. code-block:: cpp

   #include "Checker/MyChecker.h"
   #include "llvm/IR/Instructions.h"
   #include "llvm/Support/raw_ostream.h"

   using namespace llvm;
   using namespace lotus;

   char MyChecker::ID = 0;

   bool MyChecker::runOnModule(Module &M) {
       BugReportMgr &mgr = BugReportMgr::get_instance();
       bugTypeId_ = mgr.register_bug_type(
           "DangerousCall", BugDescription::BI_HIGH,
           BugDescription::BC_SECURITY, "Call to dangerous function");

       for (auto &F : M) {
           if (F.isDeclaration()) continue;
           checkFunction(F);
       }

       return false;
   }

   void MyChecker::checkFunction(Function &F) {
       for (auto &BB : F) {
           for (auto &I : BB) {
               if (auto *call = dyn_cast<CallInst>(&I)) {
                   Function *callee = call->getCalledFunction();
                   if (callee && callee->getName() == "dangerous_function") {
                       BugReportMgr &mgr = BugReportMgr::get_instance();
                       auto *report = new BugReport(bugTypeId_);
                       report->append_step(call, "Call to dangerous function");
                       mgr.insert_report(bugTypeId_, report, /*deduplicate_by_trace=*/true);
                   }
               }
           }
       }
   }

   static RegisterPass<MyChecker> X("my-checker", "My Bug Checker");

Adding a Data Flow Analysis
----------------------------

Lotus provides an IFDS/IDE solver framework under ``include/Dataflow/IFDS/``. To implement an IFDS problem:

Step 1: Define Problem Class
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``include/Dataflow/IFDS/Analyses/MyIFDSAnalysis.h``:

.. code-block:: cpp

   #pragma once

   #include "Dataflow/IFDS/Core/IFDSFramework.h"
   #include "Dataflow/IFDS/Solver/IFDSSolver.h"

   namespace lotus {

   struct MyFact {
       const llvm::Value *val = nullptr;

       bool operator==(const MyFact &other) const { return val == other.val; }
       bool operator<(const MyFact &other) const { return val < other.val; }
   };

   class MyAnalysis : public ifds::IFDSProblem<MyFact> {
   public:
       MyFact zero_fact() const override { return MyFact{nullptr}; }

       FactSet normal_flow(const llvm::Instruction *stmt,
                           const llvm::Instruction *succ,
                           const MyFact &fact) override {
           FactSet result;
           result.insert(fact);
           return result;
       }

       FactSet call_flow(const llvm::CallBase *call,
                         const llvm::Function *callee,
                         const MyFact &fact) override {
           FactSet result;
           return result;
       }

       FactSet return_flow(const llvm::CallBase *call,
                           const llvm::Instruction *exit_inst,
                           const llvm::Instruction *return_site,
                           const llvm::Function *callee,
                           const MyFact &exit_fact,
                           const MyFact &call_fact) override {
           FactSet result;
           return result;
       }

       FactSet call_to_return_flow(
           const llvm::CallBase *call,
           const llvm::Instruction *return_site,
           llvm::ArrayRef<const llvm::Function *> callees,
           const MyFact &fact) override {
           FactSet result;
           result.insert(fact);
           return result;
       }

       FactSet initial_facts(const llvm::Function *main) override {
           FactSet seeds;
           seeds.insert(zero_fact());
           return seeds;
       }
   };

   } // namespace lotus

Step 2: Solve and Query Results
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   #include "Dataflow/IFDS/Solver/IFDSSolver.h"

   MyAnalysis problem;
   ifds::IFDSSolver<MyAnalysis> solver(problem);
   solver.solve(*module);

   auto results = solver.get_all_results();

Testing and Debugging
---------------------

Writing Unit Tests
~~~~~~~~~~~~~~~~~~

Unit tests live under ``tests/unit/``, organized by subsystem (e.g., ``Analysis``, ``Checker``, ``Concurrency``, ``ControlFlow``, ``Dataflow``, ``Fuzzing``, ``IR``, ``Pointer``, ``Solvers``, ``Verification``).

Shared CMake helpers are defined in ``tests/unit/UnitTestHelpers.cmake``. Use the subsystem-specific helper (such as ``add_lotus_pointer_test`` or ``add_lotus_checker_test``) rather than generic custom targets.

Create ``tests/unit/Pointer/MyAliasAnalysisTest.cpp``:

.. code-block:: cpp

   #include "gtest/gtest.h"
   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "Alias/MyAA/MyAliasAnalysis.h"

   class MyAnalysisTest : public ::testing::Test {
   protected:
       llvm::LLVMContext context;
       std::unique_ptr<llvm::Module> module;

       void SetUp() override {
           module = std::make_unique<llvm::Module>("test", context);
       }
   };

   TEST_F(MyAnalysisTest, BasicAliasQuery) {
       myaa::MyAliasAnalysis aa;
       aa.runOnModule(*module);
       EXPECT_TRUE(true);
   }

Register the test in ``tests/unit/Pointer/CMakeLists.txt``:

.. code-block:: cmake

   add_lotus_pointer_test(MyAliasAnalysisTest
       MyAliasAnalysisTest.cpp
       LIBRARIES CanaryMyAA
   )

Run tests:

.. code-block:: bash

   cd build
   ctest --output-on-failure -R MyAliasAnalysisTest

Debugging Tips
~~~~~~~~~~~~~~

**1. Enable LLVM debug output**:

.. code-block:: cpp

   #define DEBUG_TYPE "my-analysis"
   #include "llvm/Support/Debug.h"

   LLVM_DEBUG(dbgs() << "Processing instruction: " << *I << "\n");

Run with debug output enabled:

.. code-block:: bash

   ./my-tool -debug-only=my-analysis input.bc

**2. Verify IR passes**:

.. code-block:: cpp

   #include "llvm/IR/Verifier.h"

   PM.add(llvm::createVerifierPass());

**3. GDB debugging**:

.. code-block:: bash

   gdb --args ./my-tool input.bc
   (gdb) break myaa::MyAliasAnalysis::runOnModule
   (gdb) run

Performance Optimization
------------------------

Profiling
~~~~~~~~~

Use LLVM's ``-time-passes``:

.. code-block:: bash

   ./my-tool -time-passes input.bc

Optimization Strategies
~~~~~~~~~~~~~~~~~~~~~~~

1. **Sparse Data Structures**: Use ``llvm::BitVector`` or ``llvm::SparseBitVector`` for points-to sets.
2. **Caching**: Memoize expensive queries across instruction visits.
3. **SCC Collapsing**: Merge strongly connected components in constraint or dependency graphs.
4. **Early Termination**: Check for fixpoint convergence at loop boundaries.

Contributing to Lotus
---------------------

Contribution Workflow
~~~~~~~~~~~~~~~~~~~~~

1. **Fork** the repository on GitHub
2. **Create** a feature branch: ``git checkout -b my-feature``
3. **Implement** changes with unit tests under ``tests/unit/``
4. **Test**: ``cd build && ctest --output-on-failure``
5. **Commit** with clear messages: ``git commit -m "Add feature"``
6. **Push**: ``git push origin my-feature``
7. **Create** a pull request on GitHub

Code Review Process
~~~~~~~~~~~~~~~~~~~

- Code must pass CI tests
- Maintainer approval required before merge
- Follow Lotus coding standards
- Add documentation and regression/unit tests

Documentation Updates
~~~~~~~~~~~~~~~~~~~~~

When adding features:

1. Update relevant ``.rst`` files under ``docs/source/``
2. Add API references in ``api_reference.rst``
3. Build documentation:

.. code-block:: bash

   cd docs
   make html

Common Pitfalls
---------------

Memory Management
~~~~~~~~~~~~~~~~~

- LLVM IR objects (instructions, basic blocks) are owned by their parent containers. Allocate using factory methods:

.. code-block:: cpp

   BasicBlock *bb = BasicBlock::Create(context, "entry", func);

- Do not keep raw pointers across passes that delete or reallocate IR nodes. Use ``llvm::Value::replaceAllUsesWith()`` before removing instructions.

Pass Dependencies
~~~~~~~~~~~~~~~~~

Declare dependencies in ``getAnalysisUsage()``:

.. code-block:: cpp

   void getAnalysisUsage(AnalysisUsage &AU) const override {
       AU.addRequired<DominatorTreeWrapperPass>();
       AU.setPreservesAll();
   }

Fixpoint Convergence
~~~~~~~~~~~~~~~~~~~~

Set iteration limits or check monotonicity when computing fixed points:

.. code-block:: cpp

   const int MAX_ITERATIONS = 100;
   int iter = 0;
   while (changed && iter < MAX_ITERATIONS) {
       changed = step();
       iter++;
   }
   if (iter >= MAX_ITERATIONS) {
       errs() << "Warning: Analysis reached iteration limit\n";
   }

Resources
---------

- **LLVM Documentation**: https://llvm.org/docs/
- **LLVM Programmer's Manual**: https://llvm.org/docs/ProgrammersManual.html
- **Lotus Repository**: https://github.com/ZJU-PL/lotus
- **Issue Tracker**: https://github.com/ZJU-PL/lotus/issues

See Also
--------

- :doc:`../user_guide/architecture` - Framework architecture
- :doc:`api_reference` - API documentation
- :doc:`../user_guide/tutorials` - Usage examples
