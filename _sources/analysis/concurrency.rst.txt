Concurrency Analysis
====================

Thread-aware analyses for concurrent programs.

**Headers**: ``include/Analysis/Concurrency``

**Implementation**: ``lib/Analysis/Concurrency``

**Main components**:

- **LockSetAnalysis** – Lock-set computation for potential data-race detection
- **MemUseDefAnalysis** – Memory use/def analysis in concurrent contexts
- **MHPAnalysis** – May-Happen-in-Parallel (MHP) analysis
- **ThreadAPI** – Helper utilities for modeling thread creation and synchronization

**Typical use cases**:

- Identify potentially racy memory accesses
- Reason about which instructions may execute in parallel
- Provide concurrency-aware information to higher-level analyses

**Basic usage (C\+\+)**:

.. code-block:: cpp

   #include <Analysis/Concurrency/MHPAnalysis.h>

   llvm::Module &M = ...;
   mhp::MHPAnalysis mhp(M);
   mhp.analyze();

   const llvm::Instruction *I1 = ...;
   const llvm::Instruction *I2 = ...;

   bool mayParallel = mhp.mayHappenInParallel(I1, I2);


