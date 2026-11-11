CclyzerAA
=========

``CclyzerAA`` is a Datalog-based pointer analysis integration layer for Lotus,
backed by the in-tree vendored ``third-party/cclyzerpp`` (Galois, Inc.).

**Headers**: ``include/Alias/InclusionBased/CclyzerAA/``

**Implementation**: ``lib/Alias/InclusionBased/CclyzerAA/``

**Third-Party Backend**: ``third-party/cclyzerpp/``

**CLI Tool**: ``tools/alias/lotus-alias-cclyzer-aa.cpp`` (binary ``build/bin/lotus-alias-cclyzer-aa``)

Overview
--------

``cclyzer++`` translates LLVM IR into relational Datalog facts and executes
precise, scalable pointer analysis using Soufflé. It supports:

- **Subset analysis** (Andersen-style inclusion-based)
- **Unification analysis** (Steensgaard-style equivalence-based)
- **Context sensitivity** (Call-site sensitive k-CFA and caller-sensitive modes)
- **Field & array sensitivity**
- **Indirect call resolution and call-graph construction**
- **Null-pointer detection**

Enabling the Backend
--------------------

To enable the full cclyzer++ backend:

1. Install Soufflé (compiler and C++ headers).
2. Configure Lotus with ``LOTUS_ENABLE_CCLYZER=ON``:

.. code-block:: bash

   cmake -B build -S . -DLOTUS_ENABLE_CCLYZER=ON
   cmake --build build -j

C++ API Usage
-------------

Direct API
^^^^^^^^^^

.. code-block:: cpp

   #include "Alias/InclusionBased/CclyzerAA/CclyzerAA.h"

   lotus::cclyzer::CclyzerOptions options;
   options.analysis = lotus::cclyzer::AnalysisKind::Subset;
   options.context = lotus::cclyzer::ContextKind::CallSite1;

   lotus::cclyzer::CclyzerAA aa(options);
   if (aa.run(module)) {
     auto res = aa.alias(ptr1, ptr2);
     std::vector<const llvm::Value *> pts;
     aa.getPointsToSet(ptr1, pts);
   }

Via AliasAnalysisWrapper
^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: cpp

   #include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

   auto aa = lotus::AliasAnalysisFactory::create(module, lotus::AAConfig::CclyzerAA_Default());
   if (aa->isInitialized()) {
     auto res = aa->query(ptr1, ptr2);
   }

Command-Line Tool
-----------------

.. code-block:: bash

   build/bin/lotus-alias-cclyzer-aa input.bc --analysis=subset --context-sensitivity=1-cfa --print-pts --print-cg

See also
--------

- See :doc:`alias_analysis` for the full alias-analysis landscape in Lotus.
- See :doc:`aserpta`, :doc:`sparrowaa`, and :doc:`lotusaa` for in-tree
  alternatives with dedicated front-ends.
