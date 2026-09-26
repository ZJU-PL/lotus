Value-Flow-Based Pointer Analysis
=================================

Overview
========

``lotus::alias::ValueFlowPTA`` implements the value-flow formulation from
Lian Li, Cristina Cifuentes, and Nathan Keynes, *Boosting the Performance of
Flow-sensitive Points-to Analysis using Value Flow*, ESEC/FSE 2011. It is a
context-insensitive, field-insensitive, module-level analysis alongside
``FlowSensitivePTA`` and ``VersionedFlowSensitivePTA``.

Unlike the other two solvers in this directory, ``ValueFlowPTA`` builds its
own value-flow and interprocedural control graphs from LLVM IR. It does not
consume SVFG indirect edges, require MemorySSA, or run a flow-insensitive
pre-analysis.

Public API
==========

Include ``Alias/InclusionBased/FlowSensitive/ValueFlowPTA.h``, construct the
analysis with a module that remains alive and unchanged, and call
``analyze()`` before querying it:

.. code-block:: cpp

   lotus::alias::ValueFlowPTA analysis(module);
   analysis.analyze();

   for (auto object : analysis.getPointsTo(pointer)) {
     if (object == analysis.getUnknownObjectId())
       continue; // Wildcard object: may denote any memory object.
     if (object == analysis.getNullObjectId())
       continue; // Null is distinct from unknown.
     const llvm::Value *allocation = analysis.getObjectValue(object);
   }

``getObjectId`` maps an allocation, global, function, modeled allocation call,
or by-value formal argument to its analysis-local object ID. Zero means that
there is no corresponding object. ``getPointedToBy`` provides the inverse
mapping for LLVM pointer values. ``mayAlias`` treats unknown as a wildcard and
is conservative for untracked values.

An empty points-to set is bottom, not unknown. Query methods reject use before
``analyze()``. Re-running ``analyze()`` reconstructs the program model and
invalidates references returned by earlier queries.

Entry points
============

By default the analysis starts from a defined ``main``. If no such definition
exists, it uses externally visible definitions, falling back to all defined
functions. Pointer arguments of default entry points start as unknown.

Explicit entries can be selected with ``Config::entryPoints``:

.. code-block:: cpp

   lotus::alias::ValueFlowPTA::Config config;
   config.entryPoints = {module.getFunction("entry")};
   lotus::alias::ValueFlowPTA analysis(module, config);

Algorithm
=========

Allocations and abstract memory objects introduce address nodes. Copies,
casts, PHIs, selects, GEPs, arguments, and returns add direct value-flow edges.
Stores have distinct value nodes and loads have distinct result nodes.

The solver traverses the value-flow graph once per affected object to compute
both points-to and pointed-to-by relations. It orders indirect-flow discovery
using object escapes: when object ``B`` is stored through a pointer to ``A``,
the indirect flows of ``A`` are processed before those of ``B``. Static global
initializers participate as virtual stores.

The solver expands the interprocedural control graph at memory-access
boundaries so dominance preserves instruction order within a basic block. It
computes dominators and dominance frontiers once for each control-flow
topology. For each object, Algorithm 2's sparse graph contains its relevant
stores plus their iterated dominance-frontier nodes. Loads consume the
reaching-store set of their nearest dominating sparse definition.

The reaching-definition equations carry store-node IDs rather than complete
memory maps. A singleton store performs a strong update; non-singleton and
explicitly partial writes perform weak updates. If a scalar escape cycle or a
newly exposed strong store invalidates the fast schedule, the solver restarts
from direct edges with all stores weak. This conservative fallback is reported
by ``Statistics::solver.usedWeakFallback``.

This is Algorithm 2 inside the implementation's interprocedural supergraph,
not the paper's per-function graph with auxiliary call-boundary operations.
The LLVM-independent core also currently uses ordered ``std::set`` containers.
Consequently, the implementation does not claim the paper's benchmark-scale
performance results; those require a separate evaluation in Lotus.

Defined calls add actual-to-formal, return-to-result, call, and return edges.
Indirect calls are refined from the called pointer's points-to set; the solver
is rebuilt when refinement adds control-flow paths. Unknown callees retain a
conservative external alternative.

This interprocedural model replaces the paper's four auxiliary load/store
instructions at each by-reference call boundary with a context-insensitive
interprocedural control graph. Call and return paths are not context matched,
so infeasible cross-return paths can reduce precision.

Memory model and limitations
============================

The analysis is field-insensitive: arrays and structures are monolithic
objects, and GEPs copy the base address. Heap sites, aggregates, cyclic alloca
sites, recursive stack allocations, and by-value copies are summary objects
and do not receive strong updates.

Static global initializers, common C allocation functions, common C++ ``new``
names, pointer atomics, and LLVM memory intrinsics are modeled. ``realloc`` may
return either its input object or a fresh allocation. Opaque external calls
return unknown pointers and weakly clobber memory unless attributes rule out
writes. Deallocation does not remove points-to facts.

Partial or non-pointer writes introduce unknown pointer contents without a
kill. Zero-length memory intrinsics have no effect. Byte-precise aggregate
copies and integer/pointer representation reconstruction are not modeled.

The input must expose analyzed control flow. Nonempty ``llvm.global_ctors`` or
``llvm.global_dtors`` lists are rejected; lower them to explicit entry calls
first. The analysis assumes sequential execution and does not model hidden
transfers such as ``setjmp``/``longjmp``, dynamic loading, or arbitrary inline
assembly semantics.

Command line
============

Run the analysis through the existing driver:

.. code-block:: bash

   ./build/bin/lotus-alias-fspta input.bc --analysis=vfpta
   ./build/bin/lotus-alias-fspta input.bc --analysis=vfpta --print-pts

``--dump-stats`` reports graph, call-refinement, indirect-flow, and strong
update counters. Because this analysis builds its own graph and does not expose
per-program-point memory maps, ``--dump-svfg`` and ``--print-memory`` are not
available in ``vfpta`` mode.
