Getting Started
===============

This page covers everything needed to build Lotus from source and run your first analysis. It describes the required dependencies, CMake configuration options, build steps, and a quick-start example invoking a checker against a compiled bitcode file.

System Requirements
-------------------

Lotus has been tested on **x86/ARM Linux** and **ARM macOS**. The following dependencies are required before building.

.. list-table:: Required Dependencies
   :widths: 20 20 60
   :header-rows: 1

   * - Dependency
     - Required Version
     - Notes
   * - LLVM
     - 14.x
     - Must match exactly; the build system searches for it automatically.
   * - Z3
     - 4.11
     - SMT solver; found via ``cmake/FindZ3.cmake``.
   * - CMake
     - 3.18+
     - Build system generator.
   * - C++ compiler
     - C++17 compatible
     - GCC or Clang with ``-fexceptions -frtti``.
   * - Boost
     - 1.80+ (optional)
     - Only required when ``LOTUS_ENABLE_SEAHORN``, ``LOTUS_ENABLE_CLAM``, or ``LOTUS_ENABLE_CCLYZER`` are enabled.

Build Instructions
------------------

The standard workflow to build Lotus is to create an out-of-source build directory and invoke CMake.

.. code-block:: bash

   git clone https://github.com/ZJU-PL/lotus.git
   cd lotus
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j$(nproc)

If your LLVM installation is not in a standard system path, provide it to CMake:

.. code-block:: bash

   cmake .. -DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm

To run the unit test suite (recommended after building):

.. code-block:: bash

   make test
   # Or using ctest:
   ctest --output-on-failure

Quick-Start Example
-------------------

The standard workflow for running a Lotus checker on C/C++ code is:

1. Compile the target program to LLVM bitcode using ``clang`` with ``-emit-llvm``.
2. Run a Lotus checker tool (from ``build/bin/``) on the ``.bc`` file.

.. code-block:: bash

   # 1. Compile C code to LLVM bitcode
   clang -g -emit-llvm -c example.c -o example.bc

   # 2. Run an alias analysis pass
   ./build/bin/lotus-alias-sparrow-aa example.bc

   # 3. Run a bug-finding checker (e.g., Use-After-Free & Leak checking)
   ./build/bin/lotus-saber example.bc

Command-Line Tools Reference
----------------------------

Lotus exposes its internal libraries through standalone executable drivers in ``build/bin/``. 

.. list-table:: Common Tools
   :widths: 25 35 40
   :header-rows: 1

   * - Tool Binary
     - Source Directory
     - Typical Invocation
   * - ``lotus-check --engine=gvfa``
     - ``tools/checker/``
     - ``lotus-check --engine=gvfa target.bc``
   * - ``lotus-check --engine=pulse``
     - ``tools/checker/``
     - ``lotus-check --engine=pulse target.bc``
   * - ``lotus-check --engine=ae``
     - ``tools/checker/``
     - ``lotus-check --engine=ae target.bc``
   * - ``lotus-check --engine=saber``
     - ``tools/checker/``
     - ``lotus-check --engine=saber target.bc``
   * - ``lotus-check --engine=concur``
     - ``tools/checker/``
     - ``lotus-check --engine=concur target.bc``
   * - ``lotus-check --engine=taint``
     - ``tools/checker/``
     - ``lotus-check --engine=taint target.bc``
   * - ``lotus-dfa-ifds``
     - ``tools/dataflow/``
     - ``lotus-dfa-ifds target.bc``
   * - ``lotus-dfa-mono``
     - ``tools/dataflow/``
     - ``lotus-dfa-mono target.bc``
   * - ``lotus-dfa``
     - ``tools/dataflow/``
     - ``lotus-dfa target.bc``
   * - ``lotus-alias-sparrow-aa``
     - ``tools/alias/``
     - ``lotus-alias-sparrow-aa target.bc``
   * - ``lotus-alias-dyck-aa``
     - ``tools/alias/``
     - ``lotus-alias-dyck-aa target.bc``

For detailed instructions on extending Lotus or adding new analysis passes, see the developer documentation.
