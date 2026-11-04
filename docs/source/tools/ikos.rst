IKOS Analyzer and API Specification Tools
=========================================

Lotus bundles the full IKOS analyzer and API-spec pipeline from the IKOS
API-spec fork. The pipeline discovers
external functions in LLVM 14 bitcode, scaffolds C stubs from IR declarations,
generates candidate bodies through an LLM endpoint, and compiles and links stubs
with signature verification.

Enable and install
------------------

The full integration is optional. It uses Python 3.8+, LLVM/Clang 14, Boost
(filesystem and thread), GMP/GMPXX, TBB, and SQLite3. Enable and build it with::

   cmake -S . -B build -DLOTUS_ENABLE_IKOS=ON
   cmake --build build --target lotus-ikos -j4
   build/bin/lotus-ikos example.c
   build/bin/ikos-report output.db

``LOTUS_ENABLE_IKOS`` also enables the API-spec commands. The native targets
are ``ikos-analyzer``, ``ikos-pp``, and ``ikos-import``. Reusable libraries are
``CanaryIKOSCore``, ``CanaryIKOSAR``, ``CanaryIKOSFrontend``, and
``CanaryIKOSPreprocessor``. They build inside Lotus using C++17; no separate
IKOS checkout, Python virtual environment, or build-time package downloads are
required. Optional APRON domains can be enabled with
``-DLOTUS_IKOS_ENABLE_APRON=ON`` when APRON and its dependencies are installed.

For only the Python pipeline, without building the analyzer::

   cmake -S . -B build -DLOTUS_ENABLE_IKOS_API_SPEC=ON
   build/bin/lotus-ikos-stub --help
   build/bin/lotus-ikos-api-census --help
   build/bin/lotus-ikos-api-spec --help

If the LLVM installation used by Lotus lacks clang, specify a complete LLVM 14
tool directory with ``-DLOTUS_IKOS_LLVM_BIN=/path/to/llvm-14/bin``. To install
the full IKOS component, including the pipeline::

   cmake --install build --component ikos --prefix /desired/prefix

Use ``--component ikos-api-spec`` to install only the pipeline. Compatibility
commands ``ikos``, ``ikos-report``, ``ikos-view``, ``ikos-config``,
``ikos-scan``, ``ikos-scan-cc``, ``ikos-scan-c++``, ``ikos-scan-extract``,
``ikos-stub``, and ``ikos-api-census`` accompany a full installation.

The installed commands use a private payload under
``share/lotus/ikos-api-spec``. They do not require the original Downloads
directory or a system Python ``ikos`` package. Analysis and the pipeline use
the Python standard library; syntax highlighting in the web viewer optionally
uses Pygments.
``LOTUS_IKOS_LLVM_BIN`` overrides the configured tool path at runtime.

VS Code and clangd
------------------

Enable IKOS in the build used by the editor, then refresh its scoped index::

   cmake -S . -B build -DLOTUS_ENABLE_IKOS=ON
   cmake --build build --target ikos-index

The ``ikos-index`` target does not compile the analyzer. It extracts IKOS's
actual compilation records into ``build/ikos-index/compile_commands.json`` and
links that database under ``third-party/ikos-api-spec`` for clangd discovery.
This prevents standalone IKOS headers from inheriting unrelated Lotus compile
commands. Rerun the target after changing build options or dependency paths.

For the rest of Lotus, use ``build/compile_commands.json``. If keeping a
database in the repository root, use a symlink instead of an outdated copy::

   ln -sfn build/compile_commands.json compile_commands.json

After changing the database, run ``clangd: Restart language server`` from
VS Code's command palette. The generated databases are local, ignored files
and are excluded from installation.

Offline workflow
----------------

Compile the program with clang 14, then inspect and scaffold missing functions::

   clang-14 -g -O0 -emit-llvm -c example.c -o example.bc
   lotus-ikos-api-census ir /path/to/directory-containing-bitcode
   lotus-ikos-stub check example.bc
   lotus-ikos-stub scaffold -o stubs example.bc

Implement the generated bodies and remove their placeholder markers before
linking::

   lotus-ikos-stub link --stub-dir stubs --fail-missing -o example.stubbed.bc example.bc
   lotus-ikos example.stubbed.bc

The linker rejects unfinished placeholders and mismatched signatures by
default. Already validated database entries can be reused without an LLM call::

   lotus-ikos-api-spec prefetch -o stubs --db-dir /path/to/stubdb example.bc

Generation and validation
-------------------------

Generation uses an explicitly configured provider or compatible endpoint::

   lotus-ikos-api-spec generate -o stubs --provider deepseek --model MODEL example.bc

Set the provider's key in its documented environment variable, such as
``DEEPSEEK_API_KEY`` or ``OPENAI_API_KEY``. Provider model presets come from the
imported snapshot; use ``--model`` to select a model available to your account.
No model calls occur during configuration, installation, or the Lotus tests.

Default generation requires Linux and Bubblewrap for native differential
execution. IKOS self-checks and abstract-domain judging automatically use the
bundled analyzer in a full build. For a pipeline-only installation, supply the
matching fork via ``IKOS_PREFIX`` or ``ikos`` on PATH. An ordinary upstream IKOS
release may not implement the fork's extended memory intrinsics. Census,
scaffolding, compilation, linking, and database lookup work without that analyzer
on macOS as well as Linux. Missing sandbox support does not cause an automatic
fallback to unsandboxed execution.

Analyzer self-checks work on macOS, where the judge omits the unsupported
``RLIMIT_AS`` memory cap while retaining analyzer and subprocess timeouts.
Failed analyzer processes or missing/corrupt result databases provide no
self-check evidence.

Semantic scope
--------------

The builtin classification table describes IKOS models, not the models of every
Lotus analysis. Generated stubs use ``__ikos_*`` intrinsics, which the bundled
IKOS analyzer implements, including the fork's memory extensions. Other LLVM
bitcode consumers must understand those intrinsics to interpret their
assumptions, nondeterminism, and memory effects. This integration does not add
those semantics to other Lotus checker engines. Use ``--force-stub`` to override
IKOS builtin classification when preparing a particular function manually.

Differential tests provide evidence over sampled inputs; they do not establish
soundness for every input. Review generated assumptions for the intended analysis.

Tests and provenance
--------------------

With ``LOTUS_BUILD_TESTS=ON`` and the ``verification`` test subsystem selected::

   ctest --test-dir build -R '^ikos_' --output-on-failure

The suite includes the imported regressions and an offline workflow exercising
the actual Lotus commands, successful bitcode linking, placeholder rejection,
signature mismatch rejection, and empty-database prefetch. Full-build tests also
prove intrinsic assumptions, detect division by zero, verify that linked-stub
constraints reach callers, and exercise reporting and compiler wrappers. Tests requiring
unavailable Linux tools, IKOS, or upstream experiment fixtures report skips.

Source provenance and local modifications are recorded in
``third-party/ikos-api-spec/README.lotus.md``. The imported code retains its
NASA Open Source Agreement 1.3 license and copyright notices.
