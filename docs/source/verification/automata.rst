Nested Word Automata
====================

``VerificationAutomata`` provides finite and Büchi nested-word automata in
``lotus::verification::automata``. The library uses C++17 and has no LLVM or solver
link dependency. Public headers are under ``include/Verification/Automata``;
internal headers are under its ``Detail`` directory.

Semantics and ownership
-----------------------

An alphabet interns each letter into an integer identity and a fixed
internal/call/return partition. Composed operands must have identical alphabet
identities and partitions. A call pushes its source state; a matched return reads
that state as its hierarchical predecessor. ``Bottom`` denotes the predecessor
of a pending return and is outside the ordinary state space.

Finite acceptance uses a final state with any stack. ``WordSemantics::Closed``
additionally prohibits pending calls and returns. ``NestedRun`` stores the word,
state sequence, and matching positions; internal and pending positions use -1.
``validateRun`` replays transitions and checks those brackets.

Providers own their operands through shared pointers and are confined to one
thread. Operands must remain immutable while operators or inclusion caches use
them. State IDs are local to each provider. Successor ranges remain valid across
subsequent queries. ``successorsView`` and ``enabledLettersView`` return borrowed
``ArrayView`` ranges; the legacy vector interfaces remain available.

Basic usage
-----------

.. code-block:: cpp

   #include "Verification/Automata/NwaSearch.h"
   using namespace lotus::verification::automata;

   Alphabet alphabet;
   auto call = alphabet.intern("call", LetterKind::Call);
   auto ret = alphabet.intern("return", LetterKind::Return);
   auto a = std::make_shared<NestedWordAutomaton>(alphabet);
   auto entry = a->addState(true);
   auto callee = a->addState();
   auto exit = a->addState(false, true);
   a->addTransition(entry, call, callee);
   a->addTransition(callee, ret, exit, entry);
   auto result = isEmpty(a);
   // Word [call, return], matching positions [1, 0].

Difference and determinization
------------------------------

``PowersetDeterminizer`` hash-conses relations of (down, up) states. Calls produce
(caller, callee) pairs. Returns join the current relation with the relation saved
at the call. The empty relation is the explicit rejecting sink, including for
unmapped alphabet letters.

``difference(A, B)`` interleaves a lazy product with B's lazy determinizer. It
queries B only for letters that A can take. ``intersection`` pairs linear and
hierarchical states. ``complement`` totalizes and complements finite-word
acceptance over the supplied alphabet.

``finalIsTrap`` requests certificates on an explicit B: accepting traps are
checked against every letter and return context, while states with no accepting
suffix even in the relaxed transition graph are eliminated from determinized
relations. A final state alone is insufficient for the accepting-trap shortcut.
Sparse enabled-letter indexes avoid scanning the entire alphabet at each state.

Search and liveness
-------------------

``reachable`` saturates reachable double-deckers without bounding recursion. A
completed callee summary is replayed when a new caller context appears.
``isEmpty`` interleaves reachable prefixes and weighted balanced summaries; it
can stop at an accepting run without materializing the rest of a lazy product.

``SearchStrategy::Shortest`` minimizes transitions. ``LoopPenalizingAStar`` adds
loop-head/call penalties and supports an admissible heuristic.
``SummaryReachability`` uses FIFO scheduling on the summary agenda. Extra costs
must be nonnegative and deterministic; heuristics must be admissible and zero at
goals. Callbacks receive the input provider's state IDs and alphabet letters.

``buchiIsEmpty`` interprets final states as repeatedly visited Büchi states. It
uses accepting balanced summaries and iterative Tarjan SCCs. Its certificates
cover balanced loops and pumpable loops with pending calls; ``stackGrowth``
records the latter. ``validateLasso`` checks brackets, transition replay,
accepting visits, and preservation of the entry stack across loop repetitions.
Finite complementation does not implement Büchi complementation.

Reduction and incremental inclusion
-----------------------------------

``DelayedDeadEndRemoval::stage`` computes a finite-language liveness mask without
changing the input. It follows every productive summary derivation, preserving
alternative accepted paths. ``view`` filters on demand and ``commit`` makes one
compact copy. Cancellation during staging leaves the previous mask intact.

``directSimulation`` and ``delayedSimulation`` provide inexpensive,
context-independent preorders. ``directSimulationGame`` solves the unbounded
pushdown safety game from empty paired stacks. ``delayedSimulationGame`` carries
an outstanding-acceptance bit and solves the pushdown Büchi game using nested
fixed points and projection of priority-indexed control copies.

Use ``reduceSimulationGame`` for game-based quotienting. It proposes partitions
from the rooted preorders, then certifies the quotient by a reverse pushdown
simulation from its initial states. This checks caller contexts introduced by
merging. Invalid batches are refined. The quotient homomorphism proves the other
inclusion direction. The certificates do not require determinization or DFA
minimization. A rooted preorder alone is not a safe arbitrary-context quotient.

The explicit ``AcceptanceSemantics`` argument selects finite or Büchi
preservation. Finite runs may stop immediately, so delayed finite acceptance
requires immediate matching. ``minimizeDfa`` uses Hopcroft refinement with an
implicit rejecting sink; it rejects call/return transitions and nondeterminism.

``IncrementalInclusionCheck::checkCandidate`` checks independent candidates
against the fixed minuend. ``add`` checks inclusion in the union of accumulated
interpolants, retaining lazy caches and first trying to lift the existing
counterexample. ``compact`` prunes the remainder and releases its provider chain;
counterexamples with obsolete state IDs are invalidated.

Memory and cancellation
-----------------------

``OperationContext`` supplies an atomic cancellation token, a steady-clock
deadline, and state/relation/transition/witness/workspace limits. Cancellation and
resource exhaustion throw distinct exceptions; neither is an emptiness result.
Checks occur during exploration, refinement, formula construction, and sorting.

Built-in providers use bounded arenas for lazy compound states and caches.
Worklists use arena storage or buffers reserved during preparation. Twenty named
hot phases are instrumented through ``hotLoopObserver`` and covered by a process
C++ allocation counter, including a positive-control test and both CEGAR loops.
Normal execution in these phases performs zero process-heap allocations.
Workspace exhaustion never falls back to the heap.

Preparation, owning-result construction, legacy compatibility vectors, and
exception machinery may allocate. Custom providers and cost/profiling callbacks
must honor the view/allocation contract to retain the hot-phase guarantee.
``maxWorkspaceBytes`` is a per-workspace budget, not a global process RSS limit.
On POSIX systems, backing address space is reserved once and pages are populated
as needed. Peak RSS is measured separately by the stress test.

Verification
------------

.. code-block:: sh

   cmake --build build --target nwa_tests -j4
   ctest --test-dir build -R '^nwa_tests\.' --output-on-failure

The suite covers the a^n b^n golden language, open/closed words, independent
concrete-stack comparisons, 3,000 nested calls, all reduction families, active
cancellation, workspace exhaustion, and Büchi certificates. Separate game
checks use finite game trees and unbounded deterministic language/co-Büchi
counterexample oracles.

Two 50-round workloads exercise CEGAR. One starts with 120 states and mutually
recursive functions, subtracts singleton traces, prunes, and reduces. The other
starts with 102 states, generalizes trace prefixes, and uses difference followed
by pruning with no simulation or DFA minimization. Both recheck every eliminated
trace and audit hot-phase allocations. A separate 122-state model with 60
recursive procedures exercises both game reducers and reduces to four states.
These are regression workloads, not a run of the entire SV-COMP competition.

The external reference adapters and their build target live in
``scripts/verification/nwa_reference``. They execute unchanged algorithms from a
user-supplied Ultimate checkout; no semantic stubs are used.

.. code-block:: sh

   cmake --build build --target nwa_differential_runner -j4
   python3 scripts/verification/nwa_differential.py \
     --ultimate ~/Downloads/ultimate-dev \
     --cpp-runner build/bin/nwa_differential_runner

The script builds with local ECJ/support JARs and generates parsers from
Ultimate's grammars in a temporary directory. It records source/binary hashes,
the corpus, both outputs, and a JSON report. The default corpus compares 189,860
Boolean results across 44 automaton pairs, 419 finite words and 24 ultimately
periodic words per pair, including both game reducers. Pending returns are
covered by native tests because the Java operations generally exclude them.

Algorithm references
--------------------

* Alur and Madhusudan, `Visibly Pushdown Languages
  <https://www.cis.upenn.edu/~alur/Stoc04.pdf>`_.
* Carayol and Hague, `Saturation algorithms for model-checking pushdown systems
  <https://www.cs.rhul.ac.uk/~uxac009/files/papers/afl2014.pdf>`_, Sections 4.2–4.3.
* Ultimate ``Library-Automata`` nested-word operations and simulation game code
  were inspected as references. The C++ implementation is independently written.
