# IKOS API-spec integration in Lotus

Imported from the local `ikos-api-spec` source snapshot on 2026-10-17. The
snapshot identifies its base as IKOS 3.5. It has no Git metadata, so no upstream
commit is claimed. Original copyright notices, CONTRIBUTORS.md, LICENSE.txt,
and LICENSE.pdf are retained. The distribution uses NASA Open Source Agreement
1.3; see the included license files.

This directory contains the full C++ analyzer, abstract representation, LLVM
frontend, abstract interpretation core, Python drivers, API-spec pipeline,
native differential test harness, and upstream tests. Experiments, datasets,
containers, and local configuration are excluded. The upstream CMake files are
retained for reference; Lotus builds the native code through its own adapter
under `lib/Verification/Backends/ikos`.

Lotus integration changes:

- Added relocatable `analyzer/python/ikos/settings.py`, using LLVM 14 selected
  by Lotus or `LOTUS_IKOS_LLVM_BIN`, with native tools alongside the launchers.
- Made the web viewer find resources relative to its private Python package.
- Made the generator find the bundled helpers by default. Explicit
  `--ikos-prefix` and `--site-packages` continue to select upstream packages.
- Made tests use the bundled package; mocked tool discovery in command-building
  unit tests and explicitly skipped unavailable native/IKOS integration tests.
- Fixed the judge so an unsuccessful analyzer process or missing/corrupt result
  database cannot count as a successful self-check. On Darwin, omit the
  unsupported address-space resource limit while retaining timeouts.
- Added Lotus CMake launchers, install rules, documentation, and integration
  tests outside this directory.

See `docs/source/tools/ikos.rst` in Lotus for supported commands and limits.
`tool/api-spec/README.md` and `doc/` retain the source snapshot's documentation;
its installation paths and experiment claims are upstream context. Validation
by finite differential tests is evidence, not a proof of universal soundness.
