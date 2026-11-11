Instrumentation Passes
======================

Lotus provides LLVM transformation passes in ``include/Verification/Transform/``
and ``lib/Verification/Transform/`` for preparing programs for verification.

Available Passes
----------------

BreakCritLoops
~~~~~~~~~~~~~~

Header: ``include/Verification/Transform/BreakCritLoops.h``
Pass: ``BreakCritLoopsPass``

* Identifies basic blocks that jump back to themselves via critical edges
* Splits these blocks to improve control dependence computation
* Useful before running PDG-based slicing

DeleteUndefined
~~~~~~~~~~~~~~~

Header: ``include/Verification/Transform/DeleteUndefined.h``
Pass: ``DeleteUndefinedPass``

* Removes calls to undefined void functions
* Replaces undefined non-void functions with ``verifier.nondet.undef.*`` calls
* Preserves calls to verifier functions and standard library functions
* Useful for preparing programs with missing function definitions

InitializeUninitialized
~~~~~~~~~~~~~~~~~~~~~~~

Header: ``include/Verification/Transform/InitializeUninitialized.h``
Pass: ``InitializeUninitializedPass``

* Finds all ``alloca`` instructions in function entry blocks
* Replaces uninitialized uses with calls to ``verifier.nondet.init.*`` functions
* Useful for verification where uninitialized variables should be treated as nondeterministic

MakeNondet
~~~~~~~~~~

Header: ``include/Verification/Transform/MakeNondet.h``
Pass: ``MakeNondetPass``

* Replaces calls to specified functions (e.g., ``rand()``, ``getchar()``) with nondeterministic values
* Supports the ``--make-nondet-targets`` option for configuring target functions
* Preserves return type semantics
* Useful for abstracting away I/O and random number generation

PrepareOverflows
~~~~~~~~~~~~~~~~

Header: ``include/Verification/Transform/PrepareOverflows.h``
Pass: ``PrepareOverflowsPass``

* Finds signed integer arithmetic operations (add, sub, mul)
* Replaces them with overflow-checking intrinsics (``llvm.sadd.with.overflow``, etc.)
* Calls ``__VERIFIER_error()`` if overflow detected
* Useful for overflow property checking

Programmatic Usage
------------------

These passes can be constructed and scheduled via
``include/Verification/Transform/Instrumentation.h``:

.. code-block:: cpp

   #include "Verification/Transform/Instrumentation.h"
   #include <llvm/IR/LegacyPassManager.h>
   #include <llvm/IR/PassManager.h>

   using namespace lotus::verification::transform;

   // Legacy pass manager
   llvm::legacy::PassManager PM;
   PM.add(createInitializeUninitializedPass());
   PM.add(createMakeNondetPass());
   PM.add(createDeleteUndefinedPass());

   // New pass manager
   llvm::FunctionPassManager FPM;
   FPM.addPass(createBreakCritLoopsPass());
   FPM.addPass(createPrepareOverflowsPass());

Integration with Verification
-----------------------------

These passes are designed to prepare IR for Lotus's verification backends:

* **InitializeUninitialized**: Prepares programs for abstract interpretation (CLAM)
* **MakeNondet**: Normalizes external inputs for symbolic execution (SeaHorn, Sifa)
* **PrepareOverflows**: Exposes explicit overflow assertions for verification

See :doc:`verification_backends` for verification driver usage.
