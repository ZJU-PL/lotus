CFL Reachability Certificates
============================

The ``CFL/Certificates`` module includes positive and negative
certificates for Dyck-2, arbitrary context-free grammars, PDA/P-automaton
reachability, finite two-way nondeterministic pushdown automata (2NPDAs), exact
matrix products, and a fixed hardest-language 2NPDA table.


Build and tests
---------------

The module is enabled with ``LOTUS_ENABLE_CFL`` when Boost headers are available
(``boost::multiprecision::cpp_int``). Without Boost, its libraries, tests, and
examples are skipped. No compiled Boost library is needed by the
kernels. Use ``LOTUS_CUSTOM_BOOST_ROOT`` for a custom installation. Link
``CanaryCFLCertificates`` to use the header-only APIs, or
``CanaryCFLCertificateAdapter`` for Lotus grammar/graph inputs.

With ``LOTUS_BUILD_TESTS=ON`` and the ``cfl`` test subsystem selected::

   cmake --build build --target cfl_tests -j4
   ctest --test-dir build -R '^cfl_tests\.CFLCertificate' --output-on-failure

All ten imported regression programs and the Lotus adapter tests are registered
in the shared ``cfl_tests`` binary. Assertions remain enabled for certificate
test sources in Release builds because the original tests use assertion
expressions to invoke checks.

Lotus grammar and graph inputs
-----------------------------

.. code-block:: cpp

   #include "CFL/Certificates/LotusAdapter.h"

   namespace cert = lotus::cfl::certificates;
   auto result = cert::solve(grammar, graph, source, target);
   bool valid = cert::verify(grammar, graph, source, target, result);
   bool reachable = result.status ==
       cflcert::frontend::ReachabilityStatus::Reachable;

The adapter answers one source/target query for the grammar's start symbol.
Vertex IDs are preserved. It translates every Lotus symbol to a nonterminal
with a terminal bridge, preserving graph edges labeled by nonterminals as seed
facts. Unknown edge labels, invalid endpoints, and dimensions beyond
``int`` are rejected. Operational ``Insert/Follow/Count`` metadata is rejected;
use a declarative CFG. Verification reconstructs the query from the original
Lotus inputs and checks the attached certificate independently.

Native APIs
-----------

Include headers through their public Lotus paths:

* ``CFL/Certificates/Frontend.h``: arbitrary CFG normalization,
  ``cflcert::frontend::solveCFG`` and ``verifyCFGResult``; finite 2NPDA
  compilation, decision, and certificate construction/verification.
* ``CFL/Certificates/DyckCertificates.h``: Dyck-2 walk schemes, separator
  matrices, deterministic and randomized verification, exact matrix products.
* ``CFL/Certificates/CFGCertificates.h``: generic CFG certificates.
* ``CFL/Certificates/PDACertificates.h``: PDA/P-automaton certificates.
* ``CFL/Certificates/TwoNPDA.h``: input-product and Dyck reductions.
* ``CFL/Certificates/HardestTwoNPDA.h``: explicit fixed 2NPDA table,
  bounded simulator, and semantic recognizer.

The imported ``cflcert`` namespace is preserved. Headers use the Lotus ``.h``
extension and public include paths.

Examples
--------

With ``LOTUS_BUILD_EXAMPLES=ON``::

   cmake --build build --target DyckCertificateExample \
     CFGCertificateExample TwoNPDACertificateExample
   ./build/bin/examples/DyckCertificateExample
   ./build/bin/examples/CFGCertificateExample
   ./build/bin/examples/TwoNPDACertificateExample
