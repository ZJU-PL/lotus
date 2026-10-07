UseTraceSSA — Ordered Use-History Overlay
=======================================

Overview
========

**UseTraceSSA** is an analysis-side IR overlay built on top of the Sparse Value-Flow Graph (SVFG). It enriches existing value-flow channels with explicitly ordered use histories. 

UseTraceSSA makes the temporal execution order explicit for resources without relying on purely structural slices. A use creates a pseudo-version (history :math:`\psi`); control-flow joins create history :math:`\phi` nodes; and cycles represent loops without unrolling. Each original SVFG definition has its own channel. Local native def-use edges are routed from their consumer's after-use version so they cannot bypass earlier uses. 

Pointer addresses, memory contents, and resource state remain cleanly separated domains in this representation. 

* **Location**: ``lib/IR/UseTraceSSA/``, ``include/IR/UseTraceSSA/``
* **Namespace**: ``lotus::usetracessa``
* **Core Target**: ``CanaryUseTraceSSACore``

Representation and Core Abstractions
====================================

Temporal History
----------------

``TemporalHistory`` represents execution order once per CFG. Direct internal calls, recursion, and multiple callers share temporal ports. Instruction labels are captured in one module printing pass, preserving LLVM formatting and source locations without scanning module globals for every label.

Object Identity and Guards
--------------------------

Object identity lives entirely in ``FlowEdge::objects`` and ``GuardedEventEffect`` metadata. It is never a temporal node or a product-state dimension. Guarded effects keep separate event, certainty, and ObjectSet records supplied by upstream facts (such as points-to sets from AserPTA).

* **Unknown ObjectSet**: Evaluates to ``TOP`` and applies conservatively as *May*.
* **Empty ObjectSet**: Evaluates to ``BOTTOM`` and applies to no object.
* **Event Combination**: Applicable events combine by bitwise union. Any applicable *May* effect or ``TOP`` guard makes the combined event *May*, retaining a no-effect alternative.
* **Must vs. May**: Singleton points-to sets do not inherently imply *Must* semantics; underlying pointer analysis remains unchanged.
* Graph size is completely independent of the number of objects in a points-to set.

Query Engine
============

The ``QueryEngine`` evaluates state machine queries (e.g., uninitialized use, use-after-free) against the UseTraceSSA graph. Query outcomes are ``Found``, ``NotFound``, and ``Unknown``. A ``Found`` witness is a path in the supplied abstraction; it does not guarantee concrete path feasibility.

Object Query Modes
------------------

* **Fixed Object**: ``Query::memoryObject`` selects one constant abstract object for the whole path.
* **Symbolic Batch**: ``QueryEngine::runObjects`` evaluates a finite ``ObjectUniverse`` with LLVM bit masks and returns separate Found, NotFound, and Unknown sets. 

Both modes use product states of the form ``(FlowNodeID, AutomatonState)``. Sequential steps and matched call/return summaries intersect masks; alternative paths union them. Generic taint queries do not require same-object semantics.

Context Sensitivity
-------------------

* Queries default to context depth limit 3 (``contextLimit=3``). 
* The CLI selects call/return matching with ``--context=sensitive`` (default) or ``--context=insensitive``.
* Depth is adjusted via ``--context-limit=N|unlimited`` (alias ``--context-depth``). 
* A legacy depth 0 immediately merges older call contexts but still matches the latest call site. Use ``--context=insensitive`` to disable call/return matching entirely.

Search Budgets
--------------

Search budgets default to unlimited. They can be constrained using ``--max-product-states`` and ``--max-summary-pairs`` (which also bounds context states). Budget exhaustion preserves existing findings but turns unfinished negatives into ``Unknown``. The CLI prints budget statistics, warns if a budget is exceeded, and exits with code 2 for incomplete searches. Missing models and exhausted search budgets must not become safety claims.

Bug Detection
=============

The ``DefectDetector`` provides the C++ interface for standard checkers. It supplies rules for double-free, use-after-free, memory-leak, file-leak, taint, and unchecked-use.

.. table:: Recorded resource events for Bug Detection
   :widths: 25 35 40
   :header-rows: 1

   * - Property
     - Recorded resource events
     - Candidate-object sources
   * - Double-free
     - Allocate, Release
     - Allocate / Release
   * - Use-after-free
     - Allocate, Release, Dereference
     - Allocate / Release
   * - Memory-leak
     - Allocate, Release, Escape, Exit
     - Allocate
   * - File-leak
     - Open, Close, Escape, Exit
     - Open

Its resource scan searches all objects and sinks symbolically, returning one concrete witness and the accepted objects per sink. Allocation resets are retained for double-free and use-after-free. With finite source guards, unrelated accesses/effects are omitted and candidate enumeration is limited to source objects.

Leak Checks
-----------
Leak rules search for an acquired resource reaching a root exit without a modeled release. They track ``malloc``/``calloc`` with ``free`` and ``fopen`` with ``fclose`` through root function exits. A pointer returned from a root function is treated as escaped. Ownership transfer through globals and containers is not fully modeled, so these are candidates rather than definitive leak reports. Direct release of an acquisition result is recognized as a definite close/free.

Tools and Integration
=====================

Native Construction
-------------------

``buildUseTraceSSAFromLotusSVFG`` constructs the overlay from Lotus SVFG and the LLVM module CFG. Native construction requires an explicit ``NativeHistoryMode``. The CLI chooses this mode based on the ``--check`` flag before creating the graph. 

The ``Full`` mode is an explicit general overlay for generic queries and graph dumps.

CLI Tool
--------

The command-line tool ``lotus-ir-usetracessa`` integrates the pipeline. It builds the ICFG and SVFG, and then exports JSON/DOT, runs a structural query, or checks for potential bugs using object IDs and resource histories.

.. code-block:: bash

    # Build the tool
    cmake --build build --target lotus-ir-usetracessa
    
    # Run a use-after-free check
    build/bin/lotus-ir-usetracessa input.bc --check=use-after-free
    
    # Export UseTraceSSA to JSON
    build/bin/lotus-ir-usetracessa input.bc --format=json

Taint sources/sinks, sanitizers, and full external-call semantics require client models. Ambiguous native locations are reported as graph issues. Missing required native events produce an empty resource graph with an incompleteness issue, not a safety proof.

JSON schema 2 serializes guarded effects, using ``null`` for TOP and ``[]`` for BOTTOM. The API and CLI expose graph, query, and mask-operation statistics.

Performance and Deferred Reports
================================

Resource construction contracts empty single-successor CFG chains, computes the
live iterated dominance frontier directly, and retains native provenance only
for event sites. Immutable object guards share their sorted storage. The scalar
``LLVMHistoryBuilder::build(module)`` overload shares instruction printing and
operand slots across function definitions.

Bounded and context-insensitive queries expand products from reachable search
states, intern call strings, and coalesce pending object deltas with shared
choice proofs. Unbounded queries derive matched-call summaries from callee-entry
rows instead of computing an all-pairs local closure. Query masks use inline,
dense, sparse, or sparse-complement representations within their finite universe.

``DefectDetector::scan(kind)`` retains eager witnesses. Passing
``{false, true, false}`` retains deferred proofs, recoverable through
``scan.materialize(index, graph)`` against the unchanged graph. Passing
``{false, false, false, false}`` keeps sink/object counts and representatives
without expanding per-sink object lists. ``findAny(kind)`` stops after a valid
finding and records incomplete enumeration explicitly.

For checks, ``--quiet`` skips full instruction formatting, witness rendering,
and per-sink object-list expansion. ``--full-labels`` retains exact instruction
text with quiet output. Structured debug locations identify source sites;
array operands are not interpreted as locations. Only normal return ports
receive native resource Exit events. ``--timing`` reports construction subphases
and separates symbolic search from report generation.
