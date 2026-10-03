Verification Support Analyses
=============================

``include/Verification/Analysis/`` and ``lib/Verification/Analysis/`` collect
small helper passes used by verification-oriented pipelines.

**Main components**:

- ``ClassifyInstructionsPass`` and ``CountInstrPass`` inspect module structure.
- ``ClassifyLoopsPass`` and ``GetTestTargetsPass`` identify verification-relevant
  regions.
- ``CheckModulePass`` performs lightweight module validation.

These passes are mainly support utilities for verifier front-ends rather than
standalone end-user analyses.

Pipeline use
------------

Run these passes after loading and normalizing a module — they are registered
as legacy LLVM passes with the names ``classify-instructions``,
``classify-loops``, ``count-instr``, ``check-module``, and
``get-test-targets``, so they can be added to an ``opt``/``clam`` pipeline by
name.  Their output is best used as diagnostics and preprocessing aids: their
classifications do not by themselves establish or refute a safety property.
New frontends should reuse these common checks before adding narrowly scoped
duplicate passes.

See also :doc:`backend` and :doc:`transforms`.
