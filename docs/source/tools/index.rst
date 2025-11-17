Command-Line Tools
==================

Lotus provides various command-line tools for alias analysis, CFL reachability,
bug detection, PDG querying, abstract interpretation, and model checking.

This section focuses on the primary front-end binaries built from ``tools/``.
The documentation is organized by subdirectory to match the source structure.

For a feature-oriented walk-through, see :doc:`../tutorials` and
:doc:`../bug_detection`.

Tools by Subdirectory
---------------------

The documentation is organized to match the ``tools/`` directory structure:

.. toctree::
   :maxdepth: 1

   alias
   cfl
   checker
   mcp
   owl
   pdg-query
   verifier

Verifier Tools (Detailed Documentation)
--------------------------------------

For detailed documentation on verification frameworks in ``tools/verifier/``:

.. toctree::
   :maxdepth: 1

   verifier_clam
   verifier_sprattus
