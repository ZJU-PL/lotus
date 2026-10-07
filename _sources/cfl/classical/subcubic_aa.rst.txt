Subcubic PEG Alias Analysis
==========================

Paper and scope
---------------

Lotus implements Qirun Zhang, Xiao Xiao, Charles Zhang, Hao Yuan, and
Zhendong Su, *Efficient Subcubic Alias Analysis for C*, OOPSLA 2014
(`DOI 10.1145/2660193.2660213
<https://doi.org/10.1145/2660193.2660213>`__).

The engine computes all-pairs memory and value aliases on a pointer
expression graph (PEG). The implementation covers:

* Algorithm 1 and the transition machines in Figures 10 and 11;
* Algorithm 2, which reuses the memory phase's summaries to derive value
  aliases;
* the fast-set differences in Section 3.4; and
* the connected-component decomposition in Section 3.3.2.

The input language is the paper's assignment/dereference PEG. This specialized
API is independent of the arbitrary-grammar ``SolverSession`` contract and
has a dedicated command-line tool, ``lotus-cfl-subcubic-aa``. It operates on
graphs; it does not construct a PEG directly from C or LLVM bitcode.

Relations and input contract
----------------------------

An ``a`` edge goes from an assignment's right-hand side to its left-hand
expression. A ``d`` edge goes from an expression ``e`` to its dereference
``*e``. The corresponding reverse labels are ``abar`` and ``dbar``. With
``?`` denoting an optional path and ``*`` denoting repetition, the paper's
grammar is:

.. code-block:: text

   M -> dbar V d
   V -> (M? abar)* M? (a M?)*

``M(u, v)`` means that the expressions may denote the same memory location;
``V(u, v)`` means that they may evaluate to the same pointer value. Both
relations are symmetric. ``V`` includes every diagonal pair because the
empty path is valid. ``M`` contains only paths actually derived through
dereferences. Neither relation is transitively closed.

Every assignment target must have at least one incoming ``d`` edge. This
is the PEG property used in the paper's Lemmas 5 and 6; arbitrary labeled
graphs need not satisfy it. The core validates this property and rejects
out-of-range vertex IDs. Duplicate edges do not change the result.

The low-level ``Problem`` stores only forward assignment and dereference
edges, with reverse edges implicit. The ``LabeledGraph`` adapter requires
explicit matching reverse edges and accepts only ``a``, ``abar``, ``d``, and
``dbar``. The driver can add missing reverses when ``--bidirectional`` is
explicitly requested. Unknown labels remain errors after this transformation.

Field labels such as ``f_i``, load/store constraints, and the extended Lotus
ArrayPath/Memcpy grammar are outside this input language. The existing
``encodeCflPegGraph`` helper can supply assignment/dereference edges from
Lotus alias constraints, but any resulting field labels or violated
assignment-target conditions require a suitable frontend encoding. The
existing ``AliasClient`` also applies its own graph reductions; the new
adapter preserves the vertex IDs and graph structure supplied to it.

Algorithm 1: memory aliases
---------------------------

The engine retains seven summary relations: ``M``, ``V1``, ``V1Prime``,
``D1``, ``D1Prime``, ``D2``, and ``D2Prime``. A worklist stores newly
discovered summary facts.

Initialization derives ``dbar V d`` paths using nullable ``V``. In a
canonical expression graph this inserts the reflexive ``M`` pair for each
dereferenced expression. Lotus also supports a source with several
dereference successors: it seeds every ordered pair of those successors,
including the diagonal, to preserve the same grammar semantics.

Phase one extends the right endpoint of an ``M``, ``V1``, or ``V1Prime``
summary along the outgoing edges permitted by Figure 10. An encountered
dereference creates the pivot summary used by phase two. Phase two extends
the left endpoint according to Figure 11, and a matching dereference
produces a new ``M`` pair. Every new memory-alias fact is inserted in both
orientations.

Prime states enforce the nonconsecutive-memory-path restriction. The
transition table prevents the propagation from treating ``M`` as a
transitive relation or returning to a forbidden assignment direction.
New memory edges reenter the worklist in both orientations and initiate
further two-phase propagation.

Algorithm 2: value aliases
--------------------------

After the memory phase reaches its fixed point, Algorithm 2 closes the
existing ``V1`` relation transitively and obtains ``V1Prime`` from ``V1``
and ``M``. These summaries represent the forward paths needed by the
paper's Gamma and Upsilon reachability construction.

For each original assignment, the engine constructs the initial ``V2``
seeds using the target's ``V1``, ``V1Prime``, and ``M`` successors. Its final
join combines these **immutable initial seeds** with the completed forward
relations. Newly generated output pairs do not become additional ``V2``
join inputs. This preserves the single central assignment required by the
Upsilon construction; recursively joining new output would produce false
transitive aliases.

The public value relation combines those nontrivial paths with their
reverse orientations, the memory-alias paths, and all reflexive paths.
Selecting ``compute_value_aliases = false`` stops before this phase. Value
queries, counts, and visitors then throw ``std::logic_error`` rather than
returning incomplete information.

Fast sets and components
------------------------

Each relation has packed rows over the component's local vertex IDs. With
fast sets enabled, propagation subtracts the destination's known bits from
the source row a word at a time and visits only newly discovered elements.
The incoming relation is indexed as well, supporting phase two's reversed
endpoint propagation. ``use_fast_sets = false`` runs the same algorithm
with scalar element-by-element membership checks over the same storage.

By default, the original PEG is split into weakly connected components and
each component is solved independently. The result retains the mapping from
the original vertex IDs to component-local IDs, so queries use the original
IDs. Cross-component queries return false; isolated vertices still have
their reflexive value pair. ``decompose_components = false`` processes the
input as a single graph. The ``connected_components`` counter records the
number of independently processed graphs and ``largest_component`` records
the largest processed graph.

The implementation uses 64-bit words. A fast-set difference scans at most
``ceil(component_size / 64)`` words plus its output elements. The paper's
``O(n^3 / log n)`` bound assumes a uniform-cost RAM with word size
``Theta(log n)``. The implementation provides the corresponding
word-parallel operations on fixed-width machine words; its name does not
constitute a measured speedup or a reproduction of the paper's benchmark
results. Explicit pair enumeration also costs at least the output size.

Public API
----------

Core header
   ``CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAA.h``

Lotus graph adapter
   ``CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAAAdapter.h``

Implementation and build target
   ``lib/CFL/Classical/Solvers/Engines/SubcubicAA/``, compiled into
   ``CanaryClassicalCFL``.

The namespace is ``lotus::cfl::classical::engines::subcubic``. A minimal
core API example is:

.. code-block:: cpp

   #include "CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAA.h"

   namespace aa = lotus::cfl::classical::engines::subcubic;
   aa::Problem problem;
   problem.nodes = 4; // &x, x, &y, y
   problem.dereferences = {{0, 1}, {2, 3}};
   problem.assignments = {{1, 3}}; // y = x

   aa::Result result = aa::solve(problem);
   bool aliases = result.valueAlias(1, 3); // true

For a Lotus graph, use ``aa::toSubcubicAliasProblem(graph)`` before calling
``solve``. The returned move-only ``Result`` retains packed relations and
supports constant-time ``memoryAlias`` and ``valueAlias`` queries. It also
provides counts, per-source successor visitors, and whole-relation visitors.
Visitors receive original IDs, enumerate ordered pairs once, and stop when
the callback returns false. Counts include diagonal pairs and both
orientations. Every query and per-source visitor checks its vertex IDs.

Command line
------------

The driver accepts a positional graph path or ``--graph FILE``. Text/PEG
files use Lotus's ``source target label`` format; labeled DOT and Lotus JSON
graphs use the existing ``LabeledGraph`` parser.

.. code-block:: console

   build/bin/lotus-cfl-subcubic-aa \
     tests/regress/CFL/Classical/subcubic-aa-figure4.graph \
     --query 'a,*d' --json-stats

   build/bin/lotus-cfl-subcubic-aa \
     tests/regress/CFL/Classical/subcubic-aa-figure4.graph \
     --memory-only --query 'b,*c' --print-aliases

   build/bin/lotus-cfl-subcubic-aa --graph physical-edges.peg \
     --bidirectional --no-fast-sets --no-components

``--relation memory|value`` selects the queried and enumerated relation;
the default is value. ``--memory-only`` selects memory by default, and an
explicit ``--relation value`` is incompatible with it. ``--query LHS,RHS``
uses exact vertex names. ``--print-aliases`` lists ordered pairs sorted by
source and target names; without JSON it emits ``M`` or ``V`` followed by
the two names, separated by tabs.

``--json-stats`` emits a single JSON object with work counters, options,
relation counts, and any requested query or alias-pair array. In memory-only
mode, ``value_aliases`` is ``null`` and ``value_aliases_computed`` is false.
Vertex names are JSON-escaped. Unknown options, invalid graphs, and unknown
query vertices produce a diagnostic on stderr and exit status 1.

The supplied Figure 4 example contains 10 vertices, 7 ordered memory-alias
pairs, and 30 ordered value-alias pairs. It demonstrates ``V(a, *d)`` and
``V(*d, e)`` while ``V(a, e)`` is false.

Build and validation
--------------------

With the normal Lotus LLVM 14 and other build dependencies installed:

.. code-block:: console

   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
     -DLOTUS_BUILD_ALL_LIBRARIES=OFF -DLOTUS_BUILD_TESTS=ON \
     -DLOTUS_TOOL_FAMILIES=cfl -DLOTUS_TEST_SUBSYSTEMS=cfl
   cmake --build build --target lotus-cfl-subcubic-aa cfl_tests -j2
   ctest --test-dir build --output-on-failure \
     -R 'SubcubicAA|classical_cfl_cli_subcubic_'

Unit tests compare all M/V pairs with an independent encoding of Figure 7
run by Lotus's classical ``SolverSession``. They cover the paper example,
nontransitivity, nullable initialization with multiple dereference
successors, cycles, duplicate edges, generated valid PEGs, disconnected and
isolated vertices, word boundaries, visitor cancellation, memory-only
results, and malformed inputs. CLI regression cases check the example,
fast/scalar and decomposition modes, reverse-edge handling, argument
validation, pair enumeration, and JSON escaping.
