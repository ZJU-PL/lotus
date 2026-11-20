IFDS / IDE Engine
=================

Overview
========

The **IFDS / IDE engine** in ``lib/Dataflow/IFDS`` implements the
algorithms of Reps, Horwitz, and Sagiv for:

* **IFDS** — Interprocedural Finite Distributive Subset problems,
* **IDE**  — Interprocedural Distributive Environment problems.

Both are **context-sensitive**, flow-sensitive frameworks expressed
over an **exploded super-graph** whose nodes are pairs
``(program point, data-flow fact)``.

* **Location**: ``lib/Dataflow/IFDS``
* **Solvers**: ``IFDSSolver``, ``IDESolver``

IFDS: Set-Valued Problems
=========================

IFDS targets analyses where facts are just **elements of a finite set**
and the merge operator is set union (may-analysis).
Each analysis defines:

* a finite domain ``D`` of path facts and a distinguished zero fact,
* flow functions for:

  * normal instructions,
  * calls,
  * returns,
  * call-to-return (summary edges).

Example — Taint Analysis
------------------------

``ifds::TaintAnalysis`` is an IFDS client that tracks **tainted
variables** and **tainted memory locations**:

* facts distinguish tainted SSA values vs. tainted memory,
* normal flow propagates taint through loads, stores, arithmetic, casts,
  and GEPs,
* call / return flow map taint across function boundaries,
* call-to-return flow models configurable **sources**, **sinks**, and
  **sanitizers** using the taint configuration files in ``config/``.

IDE: Value-Enriched Problems
============================

IDE extends IFDS by attaching a **value** to each fact, forming an
environment ``D → L`` where ``L`` is a lattice.
Edge functions describe how these values are transformed along the
exploded super-graph.

IDE is useful for:

* constant propagation,
* typestate tracking,
* must-analyses where join is not simple union.

Example — Linear Constant Propagation
-------------------------------------

``IDEConstantPropagation`` associates integer-typed facts with a small
lattice:

* ``⊥`` — unreachable,
* ``k`` — known constant value,
* ``⊤`` — unknown / non-constant.

Edge functions interpret assignments, copies, and arithmetic
instructions so that, whenever both operands are constant, a constant
result is propagated; otherwise, the lattice moves toward ``⊤``.

Example — Typestate Analysis
----------------------------

``IDETypeState`` tracks **finite-state properties** such as:

* file handles (``fopen``/``fclose``),
* locks (``pthread_mutex_lock`` / ``unlock``),
* heap objects (``malloc``/``free``),
* sockets and protocol states.

Clients describe a typestate property by:

* defining named states (including error states),
* registering transitions for operations (functions, opcodes, or custom
  predicates).

Edge functions then implement these transitions over the typestate
lattice, and the IDE solver yields, at each program point, the
abstract state of tracked objects.

Usage
=====

At a high level, an IFDS/IDE analysis is instantiated and solved as:

.. code-block:: cpp

   auto model = TaintConfigParser::parse_file("config/taint.spec");
   ifds::AnalysisSession::Options options;
   options.entry_points = {"main"};
   options.call_graph = lotus::CallGraphAnalysisType::OTF;
   auto session = std::make_shared<ifds::AnalysisSession>(module, options,
                                                        model.get());
   ifds::TaintAnalysis problem({}, *model);
   ifds::IFDSSolver<ifds::TaintAnalysis> solver(problem);
   solver.set_analysis_session(session);
   solver.get_solver_config().set_sparse_execution();
   solver.solve(module);

Analysis Sessions and Graph Providers
=====================================

``AnalysisSession`` lazily shares alias analysis, a debug-info type hierarchy,
and an instruction graph between IFDS/IDE problems. Its taint model is an
immutable copy of the supplied configuration. IFDS taint and extended IDE taint
consume that model; creating either client does not change global configuration.
``SANITIZER function_name`` records a sanitizer in a taint specification.

The session supports ``NORESOLVE``, ``CHA``, ``RTA``, ``VTA`` and ``OTF`` graph
construction. OTF also includes indirect targets discovered by the selected
alias backend. Indirect target sets have unknown completeness and retain an
unknown target for bypass and summary modeling. A custom ``CalleeProvider`` can
certify a complete target set using ``CalleeTargets::complete``. Null entries
always represent unknown targets.

Use ``solver.set_icfg(shared_icfg)`` or ``solver.set_callee_provider(provider)``
to inject graph services directly. Explicit providers override session graph
selection. Path-aware solvers expose the same interfaces. Custom ICFGs supply
their own return sites; the default graph retains normal and exceptional
``invoke`` continuations.

Session entry points control the default seeds; explicit client seed overrides
remain authoritative. ``model_global_initializers`` and
``model_external_callbacks`` enable existing Lotus runtime models. These options
rewrite the module during session construction, so construct the session before
other analyses or retaining instruction pointers. Both are disabled by default.

The module and LLVM context must outlive the session, graphs and solvers.
Sessions are not thread-safe. After IR mutation, call ``session->invalidate()``
and solve again; earlier results are stale. Cache invalidation preserves ownership
of already retained service snapshots. An explicitly supplied problem alias
analysis takes precedence over the session service for that problem's queries.

Sparse Execution
================

``set_sparse_execution(true)`` enables conservative fact-specific sparsification
inside basic blocks. A client supplies ``sparse_fact_value(fact)`` and a
side-effect-free ``is_identity_flow(inst, succ, fact)`` certificate. The
certificate asserts the entire flow is exactly ``{fact}`` with no other effects.
IDE additionally requires ``is_identity_edge(inst, succ, fact)`` to certify the
edge function is identity. Default certificates disable skipping.

The implementation preserves calls, block entries, terminators, branches and
custom CFG boundaries. IFDS scalar taint and extended IDE taint provide supported
certificates; unsupported facts and clients use dense propagation. Memory taint
in the IFDS client remains dense. Per-instruction facts, IDE values and immediate
ESG transitions remain available at skipped instructions.

``get_steps_performed()`` counts processed worklist jobs;
``get_sparse_transfers()`` counts certified transfers handled without a job.
Step bounds therefore count work differently in dense and sparse mode; bounded
runs may produce different partial results. Sparsification retains result tables
and does not promise a general memory reduction.

.. code-block:: bash

   build/bin/lotus-dfa-ifds input.ll --analysis=taint \
     --taint-config=config/taint.spec --call-graph=otf --sparse --statistics --stdout

The dense/sparse unit tests compare findings on an identity-heavy synthetic
fixture and check facts and values at every instruction while preserving
summaries and ESG edges. Timing, job-count, and RSS measurements require an
external benchmark harness; no such script ships with the repository.

Command-Line Tool: lotus-check --engine=taint-ifds
==================================================

The ``lotus-check --engine=taint-ifds`` frontend provides an interprocedural taint analysis using the
IFDS framework.

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint-ifds <input bitcode file> [options]

Key Options
-----------

* ``--checks=taint-flow`` – Select the taint-flow checker. It is also the
  default because this engine currently exposes one checker.
* ``--taint.sources=<functions>`` – Comma-separated list of custom source functions.
* ``--taint.sinks=<functions>`` – Comma-separated list of custom sink functions.
* ``--taint.sparse`` – Enable certified sparse transfers with the shared OTF
  call graph and the selected alias backend.
* ``--verbose`` – Show module and source/sink tagging details.
* ``--analysis-stats`` – Print analysis statistics.

For the checker-facing usage, reporting behavior, and distinctions from the
taint tracking embedded in other engines, see :doc:`../checker/taint`.

The tool performs interprocedural taint analysis to detect potential security
vulnerabilities where tainted data (from sources like user input) flows to
dangerous sink functions (like system calls, memory operations, etc.).

Examples
--------

.. code-block:: bash

   # Basic taint analysis
   ./build/bin/lotus-check --engine=taint-ifds input.bc

   # Custom sources and sinks
   ./build/bin/lotus-check --engine=taint-ifds input.bc --taint.sources=read,scanf --taint.sinks=system,exec

For other command-line tools that build on IFDS/IDE, see
:doc:`../tools/checker/index`.

Shared graph services
=====================

``AnalysisSession`` lazily shares the following immutable derived objects:

* ``function_index()``: dense ``FunctionId`` values in module order, including
  declarations and functions unreachable from the configured entry points.
* ``call_graph_sccs()``: SCC membership, members, callee/caller condensation
  edges, and cycle flags. Component IDs are sinks first: a caller component
  has a greater ID than its distinct callee components.
* ``used_globals()``: per-function direct references and transitive SCC summaries.
* ``call_graph_reachability()``: an interval index sharing that SCC decomposition.

.. code-block:: cpp

   auto functions = session.function_index();
   auto id = functions->id(module.getFunction("main"));
   if (id) {
     auto scc = session.call_graph_sccs()->componentOf(*id);
     const auto &usage = session.used_globals()->transitive(*id);
     bool might_use_g = usage.mayUse(module.getGlobalVariable("g"));
   }

Global summaries retain constants and non-pointer globals. They collect named
references through constant expressions, aliases and global initializers, and
propagate them along known call edges. ``may_access_unknown_globals`` records
unresolved calls, memory-accessing declarations and unresolved pointer-based
memory accesses; it propagates to callers. ``mayUse`` returns true for any
non-null global when that flag is set. The reference set alone must not be used
to rule out effects in such cases. These are usage summaries, not precise
read/write, alias or points-to summaries.

The call-graph index answers structural reachability through known edges only.
A negative answer cannot exclude paths through unknown targets. No unknown
callee is converted into a fabricated SCC vertex.

All services follow the existing session lifetime contract: the module and LLVM
context outlive users; IDs and results belong to one snapshot. After any IR
mutation, call ``invalidate()`` and reacquire every derived service. Retaining a
shared pointer keeps an old object alive but does not make it valid after IR
mutation. Session service construction is not thread-safe. Completed snapshots
can be queried read-only while the module remains unchanged.
