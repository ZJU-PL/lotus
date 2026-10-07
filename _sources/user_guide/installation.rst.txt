Installation Guide
===================

Installation guide for Lotus and its dependencies.

Prerequisites
-------------

* LLVM 14.0.0
* Z3 4.11
* CMake 3.18+
* C++17 compatible compiler
* Boost 1.65+ (optional — only needed when CLAM, SeaHorn, Cclyzer++, or FPsolve are enabled;
  auto-downloaded if not found)

Building Lotus
--------------

.. code-block:: bash

   git clone https://github.com/ZJU-PL/lotus
   cd lotus
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j

.. note::

   After configuration, CMake prints a **build summary** showing all enabled
   features. Run ``cmake -S . -B build`` (no flags) for a quick overview.

Configuration Options
---------------------

Lotus exposes its project-owned CMake toggles through ``cmake/LotusOptions.cmake``.
Options use a consistent ``LOTUS_*`` naming scheme.

**Core toggles:**

* ``-DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm``: Only needed if CMake cannot
  find a supported LLVM automatically.
* ``-DLOTUS_BUILD_TESTS=ON``: Build unit tests (default: OFF)
* ``-DLOTUS_BUILD_EXAMPLES=ON``: Build examples (default: OFF)
* ``-DLOTUS_ENABLE_COVERAGE=ON``: Instrument Lotus and its tests for LLVM source
  coverage (requires ``LOTUS_BUILD_TESTS=ON`` and Clang/AppleClang; default: OFF)

**Optional verifier integrations** (all OFF by default — opt-in due to heavyweight dependencies):

* ``-DLOTUS_ENABLE_CLAM=ON``: Enable CLAM abstract interpretation framework
* ``-DLOTUS_ENABLE_SEAHORN=ON``: Enable SeaHorn
* ``-DLOTUS_ENABLE_SMACK=ON``: Enable SMACK LLVM-to-Boogie verifier frontend
* ``-DLOTUS_ENABLE_SVF=ON``: Enable SVF
* ``-DLOTUS_ENABLE_HORN_ICE=ON``: Build ICE learning tools for CHC
* ``-DLOTUS_ENABLE_SEAL=ON``: Build the Seal symbolic automata lifter (CAV 2026)

**Optional in-tree tool families and components:**

* ``-DLOTUS_ENABLE_CFL=OFF``: Disable CFL reachability solvers (default: ON)
* ``-DLOTUS_ENABLE_CSR=ON``: Build the indexing context-sensitive reachability solver (default: OFF)
* ``-DLOTUS_ENABLE_OWL=ON``: Build the Owl SMT solver (default: OFF)
* ``-DLOTUS_ENABLE_SMT_STABILIZER=ON``: Build the SMTStabilizer SMT-LIB
  normalization library and tool (needs GMP, GMPXX, and MPFR; default: OFF)
* ``-DLOTUS_ENABLE_DYNAA=ON``: Build dynamic alias-analysis tools (default: OFF)
* ``-DLOTUS_ENABLE_CCLYZER=ON``: Enable optional cclyzer++ alias analysis backend (default: OFF)
* ``-DLOTUS_ENABLE_TYPE_QUALIFIER=ON``: Enable the TypeQualifier uninitialized-data checker (default: OFF)
* ``-DLOTUS_ENABLE_FPSOLVE=ON``: Build vendored FPsolve fixed-point solver (default: OFF)
* ``-DLOTUS_ENABLE_WALI_OPENNWA=ON``: Build the vendored WALi/OpenNWA library
  and enable the runtime ``wali-fwpds`` and ``wali-swpds`` WPDS backends (default: OFF)
* ``-DLOTUS_ENABLE_PDAAAL=ON``: Build the vendored PDAAAL weighted PDS reachability library
  under third-party/PDAAAL (default: OFF)

**Advanced toggles:**

* ``-DLOTUS_DOWNLOAD_BOOST=OFF``: Disable Boost auto-download (default: ON)
* ``-DLOTUS_DOWNLOAD_CRAB=ON``: Allow CRAB auto-download (default: OFF)
* ``-DLOTUS_CUSTOM_BOOST_ROOT=/path/to/boost``: Path to a custom Boost installation
* ``-DLOTUS_CUSTOM_CRAB_ROOT=/path/to/crab``: Path to a custom CRAB checkout
* ``-DLOTUS_SEADSA_ENABLE_SANITY_CHECKS=ON``: Enable Sea-DSA sanity checks (default: OFF)
* ``-DLOTUS_WPDS_WITNESS_TRACE=ON``: Enable WPDS witness tracing (default: OFF)
* ``-DLOTUS_SEAHORN_BUILD_32_BIT_RT=ON``: Build 32-bit SeaHorn runtime libraries (default: OFF)
* ``-DLOTUS_EGRAPH_ENABLE_DOT=ON``: Enable DOT/Graphviz helpers in the EGraph library (default: ON)
* ``-DLOTUS_EGRAPH_ENABLE_JSON=ON``: Enable JSON serialization helpers in the EGraph library (default: ON)

Typical configurations:

.. code-block:: bash

   # Lean local build (the defaults exclude tests and CLAM)
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

   # Full build, including tests and CLAM
   cmake -S . -B build-full -G Ninja \
     -DLOTUS_BUILD_TESTS=ON \
     -DLOTUS_ENABLE_CLAM=ON

   # Enable optional tool families
   cmake -S . -B build \
     -DLOTUS_ENABLE_DYNAA=ON \
     -DLOTUS_ENABLE_HORN_ICE=ON \
     -DLOTUS_ENABLE_CFL=ON \
     -DLOTUS_ENABLE_CSR=ON

   # Enable Seal symbolic automata lifter
   cmake -S . -B build -DLOTUS_ENABLE_SEAL=ON

Faster development builds
-------------------------

By default, Lotus builds every configured library and tool family. For focused
development, keep all library targets available but build only those needed by
the selected tools and tests:

.. code-block:: bash

   cmake -S . -B build-concurrency -G Ninja \
     -DCMAKE_BUILD_TYPE=Debug \
     -DLOTUS_BUILD_ALL_LIBRARIES=OFF \
     -DLOTUS_TOOL_FAMILIES= \
     -DLOTUS_BUILD_TESTS=ON \
     -DLOTUS_TEST_SUBSYSTEMS=concurrency \
     -DLOTUS_ENABLE_CFL=OFF
   cmake --build build-concurrency --parallel 8
   ctest --test-dir build-concurrency --output-on-failure

``LOTUS_TOOL_FAMILIES`` is a semicolon-separated list of ``alias``, ``checker``,
``dataflow``, ``optimization``, ``solver``, ``verifier``, ``ir``, and ``cfl``.
Its default is ``all``; an empty value builds no tools. Existing feature switches
still control optional tools within a selected family.

``LOTUS_TEST_SUBSYSTEMS`` selects ``alias``, ``alias-wrapper``, ``analysis``,
``checker``, ``cfl``, ``concurrency``, ``dataflow``, ``fuzzing``, ``ir``,
``solvers``, ``symbolicexecution``, ``utils``, and ``verification``. Its default
is ``all``. Quote multiple selections, for example
``-DLOTUS_TEST_SUBSYSTEMS="concurrency;alias-wrapper"``. The ``alias-wrapper``
suite checks wrapper availability without compiling all alias-analysis tests.
CLI regression tests are registered only when their tools are configured.

For finer selection, use ``subsystem/group`` in the same option. A bare subsystem
includes all of its groups; each selected subsystem still produces one test
binary, so full builds do not acquire a separate link step for every group.

.. code-block:: bash

   cmake -S . -B build-focused -G Ninja \
     -DLOTUS_BUILD_ALL_LIBRARIES=OFF \
     -DLOTUS_TOOL_FAMILIES= \
     -DLOTUS_BUILD_TESTS=ON \
     '-DLOTUS_TEST_SUBSYSTEMS=concurrency/cuda;dataflow/vasco;analysis/cfg'
   cmake --build build-focused --target lotus_unit_tests --parallel 8
   ctest --test-dir build-focused --output-on-failure

Supported groups:

* ``alias``: ``gpg``, ``dda``, ``aserpta``, ``bootstrapaa``, ``flowsensitive``,
  ``lotusaa``, ``sparrowaa``, ``tpa``, ``cclyzeraa``, ``dyckaa``, ``seadsa``,
  ``allocaa``, ``typequalifier``, ``underapproxaa``, ``ptsset``.
* ``analysis``: ``cfg``, ``controldependence``, ``debuginfo``, ``general``,
  ``multiplicity``, ``nullpointer``, ``parametersummary``, ``profile``,
  ``purity``, ``sccp``, ``typehierarchy``, ``loop``.
* ``checker``: ``ae``, ``concurrency``, ``framework``, ``kint``, ``pulse``,
  ``saber``, ``reports``.
* ``concurrency``: ``mhp``, ``lockset``, ``valueflow``, ``thread``, ``clocks``,
  ``threadapi``, ``threadlocal``, ``openmp``, ``mpi``, ``cuda``, ``linuxkernel``.
* ``dataflow``: ``ifdside``, ``mono``, ``wpds``, ``apa``, ``npa``, ``vasco``.

Source manifests and link dependencies are restricted to the selected groups.
TypeHierarchy and Loop IR fixtures are configured and generated only when those
analysis groups are selected. Production libraries retain their own dependencies;
for example CUDA tests still need the aggregate Concurrency library.

Non-template functions in ``TestUtils/LLVMHelpers.h`` are implemented in a shared
test support library, so each test source does not compile their bodies again.
Templates remain in the header. KINT's shared fixture similarly compiles its
PassBuilder initialization once rather than exposing it through every test
source. The common test targets no longer link LLVMPasses; groups that use
PassBuilder request that library explicitly. Suites with a custom entry point
use ``CUSTOM_MAIN`` to omit ``GTest::gtest_main``.

With ``LOTUS_BUILD_ALL_LIBRARIES=OFF``, CMake follows target dependencies rather
than compiling every library. You can still request any configured library with
``cmake --build build-concurrency --target SVFG``. Use the default full-library
configuration for packaging/installing the complete project. Disabling
``LOTUS_ENABLE_CFL`` removes CFL libraries, tools, and their tests; the wrapper's
LLVM-provided CFL alias analyses remain available.

The unified alias wrapper can omit heavier backends independently:

.. code-block:: bash

   cmake -S . -B build-concurrency \
     -DLOTUS_AA_WRAPPER_ENABLE_DDA=OFF \
     -DLOTUS_AA_WRAPPER_ENABLE_TPA=OFF \
     -DLOTUS_AA_WRAPPER_ENABLE_GPG=OFF \
     -DLOTUS_AA_WRAPPER_ENABLE_CCLYZER=OFF

These switches default to ON and affect only the wrapper. Standalone backend
libraries and tools remain available. SparrowAA, DyckAA, UnderApprox, Combined,
and LLVM CFL backends keep their existing behavior. Requesting an omitted
backend produces a diagnostic, leaves ``isInitialized()`` false, and returns
conservative query results. Enable the backends your chosen analysis needs;
these switches do not remove an independent dependency on a backend elsewhere.
Consumers of the wrapper header must include the generated build include
directory as well as the source include directory; linking its CMake target
supplies that directory automatically.

Profiling and optional compilation acceleration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Enable compiler profiling before choosing precompiled headers (PCH) or unity
builds:

.. code-block:: bash

   cmake -S . -B build-concurrency -DLOTUS_ENABLE_TIME_TRACE=ON
   cmake --build build-concurrency --parallel 8

Clang/AppleClang write ``*.cpp.json`` files beside their object files. Inspect
them in a trace viewer to distinguish header parsing and template instantiation
from code generation. This option applies to Lotus C++ targets, not vendored
libraries, and rejects unsupported compilers.

If repeated header parsing dominates, select individual targets for private
PCHs of stable LLVM/STL headers:

.. code-block:: bash

   cmake -S . -B build-concurrency \
     '-DLOTUS_PCH_TARGETS=Concurrency;SVFG'

Each target builds its own PCH with its own flags and definitions. Measure the
PCH creation cost as well as subsequent source compilation; small targets may
not benefit. PCH is disabled by default.

Unity builds are an opt-in experiment for compatible targets:

.. code-block:: bash

   cmake -S . -B build-concurrency \
     -DLOTUS_UNITY_TARGETS=CanaryAliasPtsSet \
     -DLOTUS_UNITY_BATCH_SIZE=4

Combining source files can expose local-name collisions or increase the cost
of rebuilding one edited file. Select targets explicitly, verify their tests,
and compare clean and incremental builds before adopting unity builds. Both
target lists reject unknown or non-compilable targets and exclude vendored
targets. Reset a list with ``-DLOTUS_PCH_TARGETS=`` or
``-DLOTUS_UNITY_TARGETS=`` to disable it. Reconfigure into a new build directory
when switching CMake generators.

Reducing link time
~~~~~~~~~~~~~~~~~~

Measure link steps separately from compilation before changing the linker.
Ninja records start/end times and outputs in ``.ninja_log``; executable outputs
under ``bin/tests/`` identify the test link steps. Sum-of-job durations are not
the same as the elapsed time of a parallel build.

Focused test selections reduce both the libraries linked into each executable
and the number of unrelated executables rebuilt after editing a dependency.
The default configuration keeps one executable per subsystem to avoid repeatedly
linking its shared dependency set. Shared test helpers compile once, and
LLVMPasses is linked only by suites that need it.

If LLVM itself does not need to be debugged, a Release LLVM 14 installation can
reduce the debug information embedded from static LLVM archives. Lotus can still
be built in Debug mode. Keep LLVM's RTTI configuration compatible with Lotus.

An alternative linker is another opt-in experiment. With LLD installed, Clang
on Linux can use:

.. code-block:: bash

   cmake -S . -B build-focused \
     '-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld'

On macOS, select the Mach-O linker executable explicitly:

.. code-block:: bash

   cmake -S . -B build-focused \
     '-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=/path/to/ld64.lld'

Retain any existing linker flags when adding these options and verify the
resulting executables. See `LLD documentation <https://lld.llvm.org/>`_ and
`the Mach-O port <https://lld.llvm.org/MachO/index.html>`_. Linking against a
shared LLVM library may reduce repeated static linking, but requires an LLVM
installation providing that library and corresponding changes to Lotus's
component-library links; simply appending ``LLVM`` to the current static link
list does not implement that change.

Z3 Installation
---------------

.. code-block:: bash

   # Ubuntu/Debian
   sudo apt-get install libz3-dev

   # macOS with Homebrew
   brew install z3

   # Or build from source
   git clone https://github.com/Z3Prover/z3.git
   cd z3 && python scripts/mk_make.py
   cd build && make && sudo make install


Troubleshooting
---------------

* **LLVM not found**: Install LLVM 14.x via your package manager (or from source).
  If you use a non-standard installation location, set ``LLVM_BUILD_PATH`` to the directory
  that contains ``LLVMConfig.cmake`` and re-run CMake.
* **Z3 not found**: Install Z3 or set ``Z3_DIR``
* **Boost issues**: Use ``LOTUS_CUSTOM_BOOST_ROOT`` or let Lotus auto-download Boost
* **Build errors**: Use supported LLVM version (14.x)
