Analysis Framework
==================

This section covers the core analysis components and frameworks in Lotus.

Lotus provides several reusable analysis utilities and frameworks under
``lib/Analysis``. These components complement the alias analyses and
high-level analyzers such as CLAM (numerical abstract interpretation) and
Sprattus (symbolic abstraction).

Overview
--------

At a glance:

- **CFG** (``lib/Analysis/CFG``): Control Flow Graph utilities for reachability,
  dominance, and structural reasoning. See :doc:`cfg`.
- **Concurrency** (``lib/Analysis/Concurrency``): Thread-aware analyses for
  multi-threaded code (MHP, lock sets, thread modeling). See :doc:`concurrency`.
- **GVFA** (``lib/Analysis/GVFA``): Global value-flow engine for interprocedural
  data-flow reasoning. See :doc:`gvfa`.
- **NullPointer** (``lib/Analysis/NullPointer``): A family of nullness and
  null-flow analyses. See :doc:`null_pointer`.
- **Sprattus** (``lib/Analysis/Sprattus``): Symbolic abstraction framework for
  abstract interpretation with composable domains. See :doc:`sprattus`.

Higher-level analyzers such as CLAM and Sprattus build on these components;
see :doc:`../tools/verifier/clam/index` and :doc:`../tools/verifier/sprattus/index` for
details.

.. toctree::
   :maxdepth: 2

   cfg
   concurrency
   gvfa
   null_pointer
   sprattus