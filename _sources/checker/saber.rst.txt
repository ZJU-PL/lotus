Saber Checker
=============

The ``Saber`` subsystem implements source-sink bug checking on top of Lotus IR
and value-flow graphs.

**Headers**: ``include/Checker/Saber/``

**Implementation**: ``lib/Checker/Saber/``

**Frontend**: ``lotus-check --engine=saber`` implemented by ``tools/checker/lotus-check-saber.cpp``

Overview
--------

Saber-style analysis tracks source-sink relationships over the sparse
value-flow graph to detect resource-management bugs. In the current tree it is
used primarily for memory leaks, double-free bugs, and file-descriptor leaks.
The use-after-free extension pairs SVFG object facts with an on-demand,
context-bounded ICFG order query.

Main components
---------------

- ``LeakChecker`` checks unmatched allocations and partial leaks.
- ``DoubleFreeChecker`` reports repeated frees on the same path.
- ``FileChecker`` handles file-descriptor style resources.
- ``UseAfterFreeChecker`` reports free-to-dereference candidates across
  functions using matched call/return sites up to the context limit.
- ``SaberCheckerAPI`` and ``SrcSnkSolver`` expose reusable source-sink solving
  infrastructure.

Typical usage
-------------

.. code-block:: bash

   ./build/bin/lotus-check --engine=saber input.bc
   ./build/bin/lotus-check --engine=saber input.bc --checks=all
   ./build/bin/lotus-check --engine=saber input.bc --checks=double-free,file-leak
   ./build/bin/lotus-check --engine=saber input.bc --checks=use-after-free

Behavior
--------

- With no ``--checks`` option, all Saber checks run.
- When multiple checks are enabled, the tool tries to build and reuse shared
  SVFG and ICFG state across checkers.
- Saber tuning parameters are explicitly namespaced, for example
  ``--saber.context-limit``, ``--saber.max-forward-items``, and
  ``--saber.solver-timeout-ms``.
- The default call-string limit is 3. A limit of 0 immediately merges contexts;
  it does not request unbounded context sensitivity. Report this setting when
  comparing Saber with analyses that use unbounded call/return matching.
- ``--saber.no-smt`` skips SMT path-condition propagation and solving for the
  original Saber source/sink checks. It retains value-flow traversal and reports
  conservative candidates: a reachable pair of frees can be reported even
  when their branches are mutually exclusive; a reachable close/free does
  not prove all paths are covered. The default retains SMT filtering for those
  checks. The use-after-free extension always reports path-insensitive
  candidates; it does not invoke SMT in either mode.

Interpreting findings
---------------------

Saber reports source-to-sink relationships that satisfy its value-flow model.
Review the diagnostic trace together with the modeled allocation and library
semantics before treating a report as confirmed.  Calls without available
bodies or summaries can affect precision, so use the shared annotation and
alias configuration consistently when comparing runs or triaging results.

Scope and alternatives
----------------------

Saber checks memory leaks, double frees, file-descriptor leaks, and
use-after-free candidates. The use-after-free extension checks load/store
dereferences and ordinary ``free`` calls, with bounded call context; it does
not prove branch/path feasibility. Use ``ae``, ``pulse``, ``fitx``, or ``symex``
for other memory-safety checks or stronger UAF validation. See
:ref:`Choosing a Checker <choosing-a-checker>` for engine selection.

See also
--------

- See :doc:`../tools/checker/index` for the tool overview.
- See :doc:`ae`, :doc:`pulse`, and :doc:`fitx` for other memory-safety checker
  families.
