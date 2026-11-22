Conditional Dyck Reachability (ConCRA)
======================================

ConCRA precomputes a library's behavior while client callbacks and selected
library edges are still unknown. The resulting conditional summary is reused
when analyzing each client. This component implements the ESOP 2017 algorithms
by Hao Tang, Di Wang, Yingfei Xiong, Lingming Zhang, Xiaoyin Wang, and Lu Zhang,
*Conditional Dyck-CFL Reachability Analysis for Complete and Efficient Library
Summarization* (`paper and artifact
<https://xiongyingfei.github.io/publication/ESOP17.html>`_).

**Locations:** ``include/CFL/ConditionalDyck/`` and
``lib/CFL/ConditionalDyck/``. Link ``CanaryConditionalDyck`` and include
``CFL/ConditionalDyck/ConditionalDyck.h``. The public namespace is
``lotus::cfl::conditional_dyck``. The target needs only C++17 and the standard
library; it does not require Soot, Java, LLVM IR, or an external solver.

Algorithm and guarantees
------------------------

* **ConCRA-f** keeps unrestricted premise sets. A derived edge requires the
  union of its operands' premises, and an existing weaker premise set subsumes
  a stronger one for the same fact.
* **ConCRA-k**, for any positive ``k``, replaces an over-limit premise set with
  a premise naming the derived fact itself. It records the derivation operands
  as bridging dependencies. Summary extraction recursively retains these
  operands, including alternative derivations and dependencies with internal
  endpoints. Simply discarding over-limit edges would lose completeness.
* Client analysis activates conditional summaries only when every premise is
  satisfied. Newly derived facts can activate more summaries. Recursive
  dependencies require an actual base fact and cannot justify themselves.

Under the paper's library/application graph assumptions, both variants preserve
whole-program reachability between client nodes. ``k`` controls the work split,
not an approximation bound. ConCRA-f can have exponentially many premise sets;
small ``k`` bounds their size but can still produce large summaries. The default
is ``k=1``. Use ``Options{std::nullopt}`` for ConCRA-f. Zero is rejected.

This is one family of directed, typed Dyck parentheses. It is not an
interleaved-Dyck solver. Queries support the paper's four languages:

.. list-table:: Query languages
   :header-rows: 1

   * - Enum
     - Meaning
   * - ``Language::Balanced``
     - ``S``: balanced parentheses
   * - ``Language::Left``
     - ``L``: only unmatched opens may remain
   * - ``Language::Right``
     - ``R``: only unmatched closes may remain
   * - ``Language::Extended``
     - ``D``: unmatched closes followed by unmatched opens

All four include the empty path. Mismatched parentheses do not form balanced
paths. Node, call-site and external-condition IDs are separate 64-bit domains;
they need not be dense and are never packed into shared bit fields.

Building a library and client
-----------------------------

``Library::graph()`` holds known library edges. ``markBoundary`` assigns roles
and can assign multiple roles to one node. ``addCallback(call, return)`` declares
possible balanced reachability through client code and marks the two boundary
roles automatically. It does not assert that the callback actually propagates
a value.

``addConditionalEdge(source, target, label, condition)`` declares a library
edge enabled by an external condition. One condition may guard several edges,
such as the argument and return edges associated with an indirect-call target.
A set passed to ``analyze`` specifies exactly which external conditions hold.
Callback premises are instead discharged by reachability discovered in the
client, including recursive calls back into the library.

.. code-block:: cpp

   #include "CFL/ConditionalDyck/ConditionalDyck.h"
   using namespace lotus::cfl::conditional_dyck;

   Library library;
   library.graph().addEdge(0, 1);
   library.graph().addEdge(2, 3);
   library.markBoundary(0, Boundary::Input);
   library.markBoundary(3, Boundary::Output);
   library.addCallback(1, 2);

   Solver solver;
   const Summary summary = solver.summarize(library); // ConCRA-1

   Graph client;
   client.addEdge(10, 0, Label::open(100));
   client.addEdge(3, 11, Label::close(100));
   client.addEdge(1, 12, Label::open(200));
   client.addEdge(13, 2, Label::close(200));
   client.addEdge(12, 13); // callback propagates its input

   const Reachability result = solver.analyze(summary, client);
   const bool flows = result.contains(10, 11); // true

The client graph supplies its own edges and boundary-crossing edges. Do not
copy library-internal edges into it. The API validates crossings as follows:

.. list-table:: Boundary edges
   :header-rows: 1

   * - Direction
     - Label
     - Library endpoint role
   * - Client to library
     - Open
     - Input
   * - Library to client
     - Close
     - Output
   * - Library to callback
     - Open
     - Call
   * - Callback to library
     - Close
     - Return

Matching call-site IDs must belong to one scope: library-internal,
client-internal, a call into the library, or a callback. Reusing an ID across
these scopes is rejected. Every matching callback argument/return endpoint pair
must have a declared callback premise. Unknown external condition IDs and
invalid labels also raise ``std::invalid_argument``.

``analyze`` exports only client-to-client facts. Library nodes can be present as
boundary endpoints, but library-internal query results are not exported.
``solve(graph)`` provides ordinary whole-graph reachability for all graph nodes.
``Graph::addNode`` represents an isolated node, including its empty path.

A ``Summary`` owns its data, remains valid after its input library is destroyed,
and is immutable. Copies share read-only storage and support concurrent client
analyses. Every analysis has its own worklist and activation state. The
``statistics()`` accessor reports generated conditional facts, exported facts,
bridging facts, and the largest retained premise set.

Implementation and validation
-----------------------------

``ConCRA.cpp`` adapts the artifact's premise-priority worklist, subsumption,
bounded bridging and dependency extraction. Both variants share normalized
Dyck productions, including a private auxiliary symbol for matching calls.
``ConditionalDyck.cpp`` provides the public graph API, boundary validation,
summary ownership and indexed client-premise activation.

Tests are collected in ``cfl_tests`` through the shared Lotus test helpers:

.. code-block:: bash

   cmake --build build --target CanaryConditionalDyck cfl_tests -j4
   ctest --test-dir build -R 'cfl_tests.ConditionalDyckTest' --output-on-failure

The tests compare full and bounded summaries against an independently written
whole-graph oracle. They cover satisfied and unsatisfied callbacks, alternative
bridging derivations, recursion, shared external conditions, empty paths,
unmatched parentheses, sparse large identifiers, invalid boundary contracts,
concurrent summary reuse, and deterministic randomized graphs.

This component is a graph-level library. An LLVM analysis client must construct
the boundary nodes and conservative possible callbacks/conditional call edges
for its own analysis. This integration does not automatically extract those
models from bitcode or serialize summaries to disk.
