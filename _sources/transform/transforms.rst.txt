Transform Passes
================

Lotus provides a collection of LLVM IR transformation passes under ``lib/Transform``.
These passes are primarily used to normalize and simplify bitcode before running
analyses (alias analysis, numerical abstract interpretation, symbolic abstraction)
or to build light-weight optimization pipelines.

Overview
--------

Use these passes when you need IR that is easier for analyses and tools to consume:

- **Core normalization**: simplify PHI/select/constant expressions so downstream
  analyses do not need to handle every LLVM IR corner case.
- **Memory and data canonicalization**: make aggregates, GEPs, and global
  initializers easier to reason about.
- **Control-flow cleanup**: remove dead blocks, normalize loop latches, and give
  blocks stable names.
- **Specialized transforms**: lower vector and floating-point operations
  for analysis or targeted environments.

All passes live in ``lib/Transform`` and are exposed as standard LLVM passes.

Core Transforms
---------------

Normalization passes that rewrite IR into a simpler, analysis-friendly form.

**Location**: ``lib/Transform``

**Main passes**:

- **ElimPhi** – Convert PHI nodes to explicit select or copy operations when possible.
- **ExpandAssume** – Expand ``llvm.assume`` intrinsics into explicit branches and checks.
- **FlattenInit** – Normalize complex global initializers into flat, explicit forms (source retained; disabled in default build).
- **LowerConstantExpr** – Lower ``ConstantExpr`` nodes into instructions so analyses
  can work over a uniform instruction set.
- **LowerSelect** – Lower ``select`` instructions into equivalent branch-based control flow.
- **MergeReturn** – Merge multiple ``return`` instructions into a single exit block.

**Typical use cases**:

- Prepare bitcode for alias or numerical analyses that assume instruction-level IR.
- Simplify unusual IR patterns produced by front-ends or optimizers.
- Make control-flow and data-flow graphs easier to traverse.

**Basic usage (C\+\+)**:

.. code-block:: cpp

   #include <Transform/LowerConstantExpr.h>
   #include <llvm/IR/PassManager.h>

   llvm::Module &M = ...;
   llvm::ModuleAnalysisManager MAM;
   LowerConstantExprPass Pass;
   llvm::PreservedAnalyses PA = Pass.run(M, MAM);

Memory and Data Transforms
--------------------------

Transformations that simplify memory layout and aggregate operations.

**Location**: ``lib/Transform``

**Main passes**:

- **LowerGlobalConstantArraySelect** – Lower selects over constant global arrays
  to simpler forms.
- **MergeGEP** – Merge chained GEP instructions into a single GEP when possible.
- **SimplifyExtractValue** – Simplify ``extractvalue`` instructions on aggregates.
- **SimplifyInsertValue** – Simplify ``insertvalue`` instructions on aggregates.

**Typical use cases**:

- Canonicalize pointer arithmetic and field accesses before pointer analysis.
- Reduce the number of aggregate operations that downstream passes must understand.
- Improve readability of IR when inspecting transformed modules.

Control-Flow Transforms
-----------------------

Transformations that restructure and clean up control flow.

**Location**: ``lib/Transform``

**Main passes**:

- **RemoveDeadBlock** – Eliminate unreachable or trivially dead basic blocks.
- **RemoveNoRetFunction** – Remove calls to known non-returning functions and
  tidy up unreachable code.
- **SimplifyLatch** – Normalize loop latch structure to a canonical form.
- **NameBlock** – Assign stable, human-readable names to basic blocks.

**Typical use cases**:

- Cleanup after aggressive inlining or partial optimization.
- Prepare IR for analyses that assume canonical loop shapes.
- Improve the stability of analysis results and debug dumps.

Vector and Floating-Point Transforms
------------------------------------

Transformations for vector operations and software floating-point emulation.
For general module optimization pipelines, see
:doc:`../optimization/pass_ordering`.

**Location**: ``lib/Transform``

**Main passes**:

- **SoftFloat** – Header interface for replacing hardware floating-point operations
  with software implementations (defined in ``include/Transform/SoftFloat.h``;
  currently not built into ``CanaryTransform``).
- **UnrollVectors** – Unroll short vector operations when profitable.

**Typical use cases**:

- Lower vector operations prior to analyses that do not model vector instructions.
- Replace floating-point operations in environments without hardware FP support.

**Pipeline usage example (C\+\+)**:

.. code-block:: cpp

   #include <Transform/LowerSelect.h>
   #include <Transform/MergeReturn.h>
   #include <Transform/MergeGEP.h>
   #include <llvm/IR/PassManager.h>

   llvm::Module &M = ...;
   llvm::ModuleAnalysisManager MAM;

   LowerSelectPass SelectLowerer;
   MergeReturnPass ReturnMerger;
   llvm::MergeArrayGEP GEPMerger;

   SelectLowerer.run(M, MAM);
   ReturnMerger.run(M, MAM);
   GEPMerger.run(M, MAM);
