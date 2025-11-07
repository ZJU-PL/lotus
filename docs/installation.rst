Installation Guide
===================

Installation guide for Lotus and its dependencies.

Prerequisites
-------------

* LLVM 12.0.0 or 14.0.0
* Z3 4.11
* CMake 3.10+
* C++14 compatible compiler
* Boost 1.65+ (auto-downloaded if not found)

Building LLVM
-------------

.. code-block:: bash

   git clone https://github.com/llvm/llvm-project.git
   cd llvm-project
   git checkout llvmorg-14.0.0  # or llvmorg-12.0.0
   mkdir build && cd build
   cmake -DCMAKE_BUILD_TYPE=Release ../llvm
   make -j$(nproc)

Building Lotus
--------------

.. code-block:: bash

   git clone https://github.com/ZJU-Automated-Reasoning-Group/lotus
   cd lotus
   mkdir build && cd build
   cmake ../ -DLLVM_BUILD_PATH=/path/to/llvm/build
   make -j$(nproc)

Configuration Options
---------------------

**Required:**
* ``-DLLVM_BUILD_PATH``: Path to LLVM build directory (required)

**Optional:**
* ``-DCUSTOM_BOOST_ROOT``: Path to custom Boost installation
* ``-DBUILD_TESTS=ON``: Enable building tests
* ``-DENABLE_SANITY_CHECKS=ON``: Enable sea-dsa sanity checks
* ``-DENABLE_CLAM=ON``: Build CLAM abstract interpretation tools
* ``-DBUILD_HORN_ICE=OFF``: Build ICE learning tools for CHC (default: OFF)
* ``-DBUILD_GRASPAN=OFF``: Build Graspan CFL-reachability solver (default: OFF)
* ``-DBUILD_DYNAA=OFF``: Build dynamic alias analysis tools (default: OFF)
* ``-DBUILD_OWL=OFF``: Build OWL SMT solver (default: OFF)

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

* **LLVM not found**: Check ``LLVM_BUILD_PATH``
* **Z3 not found**: Install Z3 or set ``Z3_DIR``
* **Boost issues**: Use ``CUSTOM_BOOST_ROOT`` or let system auto-download
* **Build errors**: Use supported LLVM version (12.x or 14.x)
