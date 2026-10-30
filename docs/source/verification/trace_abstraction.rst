Trace Abstraction
=================

``VerificationTraceAbstraction`` implements an LLVM/ICFG safety checker using
nested-word trace abstraction. It links Lotus's existing automata engine with
Z3 and follows Ultimate Automizer's transition-formula, nested-SSA, predicate
transformer, and hierarchical Hoare-checking architecture.

Usage
-----

.. code-block:: cpp

   #include "Verification/TraceAbstraction/TraceAbstractionEngine.h"
   using namespace lotus::verification::traceabstraction;
   TraceAbstractionOptions options;
   options.maxIterations = 100;
   options.timeout = std::chrono::milliseconds(30000);
   auto result = TraceAbstractionEngine(options).verify(function);

``verify(function, targetInstruction)`` checks entry to a selected instruction.
Without that argument, calls to ``__assert_fail``, ``abort``, and
``__VERIFIER_error`` are error locations. The function overload starts at that
function's entry and follows defined callees without modifying LLVM IR.
``verify(icfg, errorNode)`` starts at the graph's global root, or its function
entries when no global root exists. A null error node enables error-call
recognition. The supplied graph remains authoritative for control flow.

``Safe`` means the refined error language is empty. ``Unsafe`` includes an
instruction trace, call/return matching positions, and an SMT-replayed state
valuation. Synthetic instructions are null pointers; the LLVM module must
outlive returned instruction pointers. Assignment keys are ``name#slot@step``.
Integer values are mathematical integer numerals; pointers are logical
``Pointer(base, offset)`` values, not fabricated native addresses.
``Unknown`` includes a reason such as unsupported semantics, solver uncertainty,
invalid memory/poison-dependent replay, or resource exhaustion. Iterations count
candidate traces checked, not emptiness queries.

Unified transition formulas and integer arithmetic
-------------------------------------------------

``TransitionFormulaBuilder`` owns one canonical vocabulary: input variables,
output variables, and hierarchical input variables. Each immutable
``TransitionFormula`` contains the relation, guards, exact-replay condition,
assignments, read/write sets, and havoc information. Feasibility, predicate
construction, and Hoare checking all use this same encoding.

The encoding uses **Z3 Int and linear integer arithmetic, never SMT
bit-vectors**. Integer widths are retained through range constraints and
constant-modulus normalization. For example, unflagged 8-bit addition wraps
using arithmetic modulo 256. Signed and unsigned comparisons interpret the
same stored integer appropriately. This preserves LLVM overflow behavior
without silently replacing machine integers with unrestricted mathematical
integers. Arrays and the two-integer pointer datatype extend the vocabulary
where memory is needed.

Supported scalar operations include addition, subtraction, multiplication by a
constant, division/remainder by a constant, constant shifts, integer bitwise
operations, comparisons, selects, integer casts, and simultaneous edge PHIs.
Bitwise operations are expressed using division/modulo by powers of two.
Symbolic multiplication, symbolic divisors, and symbolic shift amounts are
explicitly unsupported by the linear-arithmetic backend. ``freeze`` of an
explicit undef/poison value produces one stable nondeterministic integer.
General poison propagation is conservatively approximated; a concrete bug must
replay without the approximated undefined operations.

Memory and procedure state
--------------------------

Following Ultimate's two-dimensional pointer model, a pointer has an integer
object base and integer byte offset. Memory uses byte arrays and separate
metadata for object length, validity, initialization, mutability, and owning
activation. Byte extraction and reconstruction obey LLVM ``DataLayout``,
including endianness, structure padding, and pointer storage size. Pointer
payload metadata preserves pointers stored in aggregates and copied as bytes;
it is not confused with the pointer's numeric offset.

The frontend supports:

* Scalar, array, and structure stack objects; GEP and pointer bitcasts.
* Initialized globals, including aggregates and pointer initializers.
* Aliased loads/stores and pointer parameters/returns between defined functions.
* ``malloc``, ``calloc``, and ``free``, including allocation-failure paths.
* LLVM ``memcpy``, ``memmove``, and ``memset`` with constant lengths up to 4096
  bytes. Overlapping ``memmove`` reads from the source pre-state.
* Whole-object lifetime start/end, with initialization epochs preventing reuse of stale
  values after lifetime restart.
* Recursive activations: fresh stack objects, caller-local restoration, and
  invalidation of callee stack objects on return.

A return restores **only local state** from its matching call. Shared memory,
allocation counters, and callee changes to globals remain from the linear
predecessor. Activation ownership detects escaped stack addresses after return.
Loads require initialized, live, in-bounds memory; their initialization may
come from different branches, not necessarily one dominating store.

Invalid or imprecisely modeled operations overapproximate their written values
for safety proofs. ``Unsafe`` requires a replay satisfying their exact-replay
conditions, so an uninitialized load or invalid pointer cannot manufacture a
concrete bug. The property remains error-location reachability; invalid memory
is not automatically reported as a separate memory-safety violation.

Nested proofs and generalization
--------------------------------

A predicate ``P_k(V,H)`` refers to current state ``V`` and the frozen state
``H`` at the call that entered the current activation. Internal steps preserve
that entry context. Calls capture the caller state:

.. math::

   P_k(V,H) \land T_{call}(V,V') \Rightarrow P_{k+1}(V',V).

For a return at position ``r`` matching call ``c``, the rule uses both linear
and hierarchical predecessors:

.. math::

   P_r(V,H) \land P_c(H,O) \land T_{ret}(V,H,V')
   \Rightarrow P_{r+1}(V',O).

Thus recursion uses nested state contexts, not a recursion-depth cutoff or
shared flat local registers. The return rule is a nonlinear Horn clause
because it has two predicate premises; its **arithmetic remains linear**.

Scalar traces use Spacer covers for this nested Horn system. Fixedpoint SAT
means reachable and UNSAT means unreachable. Array traces use Ultimate-style
SP/WP predicate construction: modular call contexts are tried first, with
forward nested-SSA projection when more caller facts are required. This avoids
the unstable array-proof path in the repository's Z3 4.12 library.

Spacer covers are independently checked by internal/call/return Hoare queries.
SP/WP sequences are checked using the sufficient predicate-transformer
obligations ``pre => WP(T, post)`` and, for returns, implication of the caller
assumption by the hierarchical predicate. Only checked obligations are cached.
Initial predicates must hold universally and final predicates must be false.

Interpolant automata retain the validated spine and construct additional
successors on demand, following Ultimate's transition-appender pattern.
Internal, call, and return edges can all be generalized. Return checks receive
both predecessor predicates. For array predicates, conservative successor
selection tries spine destinations, self-loops, and true/false states; optional
Hoare queries have a short timeout and inconclusive candidates are omitted.
The checker must outlive a generalized provider returned by the builder.

Refinement uses existing ``difference``, ``reachable``, and
``DelayedDeadEndRemoval`` APIs. A shared deadline applies to solver and automata
work. Resource exhaustion is never interpreted as emptiness.

Current boundaries
------------------

Floating point, vectors/first-class aggregates as SSA values, exceptions,
atomics, volatile accesses, arbitrary external calls, call operand bundles,
range metadata, external-global initialization contracts, and non-default
address spaces remain unsupported. Implicit parameter ABI copies (such as
``byval``) and unmodeled alias/non-null/dereferenceability contracts are rejected.
Root pointer inputs do not yet have a
user-supplied heap/precondition interface. Pointer/integer conversions and
arbitrary reinterpretation of pointer bytes require a concrete address model
and are not supported. Cross-object comparisons involving dead or one-past
pointers are approximated and require a valid replay.

This is an adaptation of the core architecture, not a port of every Ultimate
solver strategy or frontend feature. Difficult quantified array proofs may
still return ``Unknown``. There is no concurrency model in this engine.

Tests and Ultimate references
----------------------------

.. code-block:: bash

   cmake --build build --target trace_abstraction_tests
   ctest --test-dir build -R '^trace_abstraction_tests\.' --output-on-failure

The tests cover integer/overflow semantics without bit-vectors, array aliasing,
structure layout, pointer fields, shared-memory effects, allocation/lifetime,
byte copying, recursive frames, unbounded recursive return properties,
hierarchical return checks, and rejected nonlinear arithmetic.

The implementation follows these files in the local Ultimate checkout:

* ``Library-ModelCheckerUtils/.../cfg/transitions/UnmodifiableTransFormula.java``:
  input/output/assigned-variable transition representation.
* ``Library-TraceCheckerUtils/.../singletracecheck/NestedSsaBuilder.java`` and
  ``NestedInterpolantsBuilder.java``: call-entry snapshots and nested contexts.
* ``Library-ModelCheckerUtils/.../hoaretriple/IncrementalHoareTripleChecker.java``:
  separate internal, call, and hierarchical return obligations.
* ``Library-ModelCheckerUtils/.../smt/predicates/PredicateTransformer.java`` and
  ``Library-TraceCheckerUtils/.../singletracecheck/TraceCheckSpWp.java``:
  strongest-postcondition and weakest-precondition construction/projection.
* ``CACSL2BoogieTranslator/.../chandler/MemoryPointer2D.java`` and
  ``MemoryModel.java``: object/offset pointers and separated memory metadata.
* ``TraceAbstraction/.../interpolantautomata/transitionappender/``:
  on-demand, checked successor construction.
