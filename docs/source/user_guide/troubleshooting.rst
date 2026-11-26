Troubleshooting and FAQ
========================

Solutions for common issues and answers to frequently asked questions.

Installation Issues
-------------------

LLVM Version Mismatch
~~~~~~~~~~~~~~~~~~~~~

CMake can't find LLVM or finds wrong version.

.. code-block:: text

   CMake Error: Could not find LLVM
   -- or --
   Found LLVM 15.0.0 but need 14.0.0

1. Check that LLVM 14.x is installed and visible on ``PATH``:

.. code-block:: bash

   llvm-config --version

2. If multiple LLVM versions are installed, point CMake to the desired one explicitly:

.. code-block:: bash

   cmake ../ -DLLVM_BUILD_PATH=/path/to/llvm-14/lib/cmake/llvm

Z3 Not Found
~~~~~~~~~~~~

Z3 library or headers not found during build.

.. code-block:: text

   Could not find Z3 libraries
   fatal error: z3.h: No such file or directory

1. Install Z3 development package:

.. code-block:: bash

   # Ubuntu/Debian
   sudo apt-get install libz3-dev
   # macOS
   brew install z3
   # Or build from source (modern Z3 builds with CMake, not scripts/mk_make.py)
   git clone https://github.com/Z3Prover/z3
   cmake -S z3 -B z3/build -DCMAKE_BUILD_TYPE=Release
   cmake --build z3/build -j$(nproc)
   sudo cmake --install z3/build

2. Specify Z3 path:

.. code-block:: bash

   cmake ../ -DZ3_DIR=/path/to/z3/install
   # Z3_ROOT is also accepted by cmake/FindZ3.cmake

Boost Not Found
~~~~~~~~~~~~~~~

Boost libraries required for SeaHorn, CLAM, and/or CclyzerAA not found.

.. code-block:: text

   Could NOT find Boost

1. Install Boost:

.. code-block:: bash

   # Ubuntu/Debian
   sudo apt-get install libboost-all-dev
   # macOS
   brew install boost

2. Or specify custom Boost:

.. code-block:: bash

   cmake ../ -DLOTUS_CUSTOM_BOOST_ROOT=/path/to/boost

3. Lotus can auto-download Boost:

.. code-block:: bash

   cmake ../ -DLOTUS_DOWNLOAD_BOOST=ON

Compilation Errors
~~~~~~~~~~~~~~~~~~

Undefined reference errors during linking.

.. code-block:: text

   undefined reference to `llvm::PassRegistry::registerPass'

1. Ensure LLVM was built with:

.. code-block:: bash

   cmake -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON ../llvm

2. Clean and rebuild:

.. code-block:: bash

   cd build
   rm -rf *
   cmake ../
   make -j$(nproc)

Analysis Issues
---------------

Analysis Crashes
~~~~~~~~~~~~~~~~

Tool crashes with segmentation fault.

.. code-block:: text

   Segmentation fault (core dumped)

1. Run with debug build:

.. code-block:: bash

   cmake ../ -DCMAKE_BUILD_TYPE=Debug
   make
   gdb --args ./build/bin/tool input.bc
   (gdb) run
   (gdb) backtrace

2. Check input validity:

.. code-block:: bash

   opt -verify input.bc -o /dev/null

3. Common causes:

   - Malformed LLVM IR
   - Null pointer dereference in analysis
   - Stack overflow (increase limit: ``ulimit -s unlimited``)

Analysis Takes Too Long
~~~~~~~~~~~~~~~~~~~~~~~

Analysis runs indefinitely or for hours.

1. Use faster analysis mode:

.. code-block:: bash

   # For alias analysis, use context-insensitive
   ./build/bin/lotus-alias-aser-aa -analysis-mode=ci input.bc
   # For CLAM, use interval domain
   ./build/bin/clam --crab-dom=int input.bc

2. Set timeout:

.. code-block:: bash

   timeout 300 ./build/bin/tool input.bc  # 5 minutes

3. For Kint, set function timeout:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.ll --kint.function-timeout-seconds=30

4. Reduce precision:

.. code-block:: bash

   # Disable field sensitivity
   ./build/bin/lotus-alias-aser-aa -field-sensitive=false input.bc
   # Limit context depth
   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa input.bc

Out of Memory
~~~~~~~~~~~~~

Analysis exhausts system memory.

.. code-block:: text

   std::bad_alloc
   Killed

1. Increase available memory (if possible)

2. Use more scalable analysis:

.. code-block:: bash

   ./build/bin/lotus-alias-dyck-aa input.bc

3. Analyze incrementally:

.. code-block:: bash

   # Split module into parts
   llvm-extract -func=main input.bc -o main.bc
   ./build/bin/tool main.bc

4. Limit analysis scope:

.. code-block:: bash

   # Intraprocedural only
   ./build/bin/clam --crab-inter=false input.bc

False Positives
~~~~~~~~~~~~~~~

Analysis reports bugs that don't exist.

1. Use more precise analysis:

.. code-block:: bash

   # 1-CFA instead of CI
   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa input.bc
   # Zones domain instead of intervals
   ./build/bin/clam --crab-dom=zones input.bc

2. Validate with dynamic analysis:

.. code-block:: bash

   ./build/bin/dynaa-instrument input.bc -o inst.bc
   clang inst.bc build/libRuntime.a -o inst
   LOG_DIR=logs ./inst
   ./build/bin/dynaa-check input.bc logs/pts.log basic-aa

3. Add annotations to code:

.. code-block:: c

   void my_function(int *p) {
       __builtin_assume(p != NULL);  // Help analysis
       *p = 10;
   }

Missing Bugs (False Negatives)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Analysis doesn't detect known bugs.

1. Enable all checkers:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.ll --checks=all

2. Use more comprehensive analysis:

.. code-block:: bash

   # Interprocedural
   ./build/bin/clam --crab-inter --crab-check --crab-null-check --crab-uaf-check --crab-bounds-check input.bc
   # Context-sensitive taint
   ./build/bin/lotus-check --engine=taint-ifds input.bc --verbose

3. Check if bug type is supported:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint --help  # See supported checks

Tool-Specific Issues
--------------------

CLAM Issues
~~~~~~~~~~~

CLAM fails with "unsupported instruction".

.. code-block:: text

   Unsupported LLVM instruction: <instruction>

1. Preprocess bitcode:

.. code-block:: bash

   ./build/bin/clam-pp --clam-lower-unsigned-icmp \
                  --clam-lower-select \
                  input.bc -o prep.bc
   ./build/bin/clam prep.bc

2. Use suitable domain:

.. code-block:: bash

   # Some domains don't support all operations
   ./build/bin/clam --crab-dom=int input.bc  # Basic support

CLAM reports "Could not allocate memory".

1. Use less expensive domain:

.. code-block:: bash

   # Polyhedra is expensive
   ./build/bin/clam --crab-dom=zones input.bc  # Use zones instead

2. Disable tracking:

.. code-block:: bash

   ./build/bin/clam --crab-track=num input.bc  # Track only numbers

DyckAA Issues
~~~~~~~~~~~~~

DyckAA produces too many alias pairs.

1. Enable function type checking:

.. code-block:: bash

   # Keep -function-type-check-level at its default (4)

2. The analysis is very precise but conservative. This is expected behavior.

Call graph is incomplete.

.. code-block:: bash

   # Ensure indirect calls are analyzed
   ./build/bin/lotus-alias-dyck-aa -dot-dyck-callgraph input.bc

PDG Query Issues
~~~~~~~~~~~~~~~~

PDG query returns empty set unexpectedly.

1. Check node names. Function names are case-sensitive, so this query does not
   match ``main``:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY) WHERE n.name = 'Main' RETURN n

   Use the exact spelling:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n

2. Verify PDG was built:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -q "MATCH (n) RETURN n" input.bc  # Should show nodes

3. Use verbose mode:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -v -q "MATCH (n) RETURN n" input.bc

Query parser error.

1. Check query syntax. String values must be quoted, so this query is invalid:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY) WHERE n.name = main RETURN n

   Use quotes around the value:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n

2. Use interactive mode to test:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -i input.bc
   > MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n

Taint Analysis Issues
~~~~~~~~~~~~~~~~~~~~~

No taint flows detected but vulnerability exists.

1. Specify custom sources/sinks:

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint-ifds input.bc --taint.sources=my_input_func \
                      --taint.sinks=my_output_func

2. Check that source and sink functions are actually called

3. Use ``lotus-check --engine=taint-ifds`` for interprocedural flow:

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint-ifds input.bc

Common Error Messages
---------------------

"Module verification failed"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Solution**: Your LLVM IR is malformed. Verify with:

.. code-block:: bash

   opt -verify input.bc -o /dev/null

Fix the IR or recompile from source.

"Pass 'X' is not initialized"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Solution**: Initialize pass registry:

.. code-block:: cpp

   initializeCore(*PassRegistry::getPassRegistry());

"Couldn't find alias information"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Error**: Pass expects alias analysis but none provided.

**Solution**: Add alias analysis to pass manager:

.. code-block:: cpp

   PM.add(createBasicAAWrapperPass());
   PM.add(myPass);

"Out of range access" or "Vector index out of bounds"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Error**: Accessing invalid array/vector index.

**Solution**: This is usually a bug in the analysis. Report it with:

.. code-block:: bash

   # Minimal reproducible example
   clang -emit-llvm -c minimal.c -o minimal.bc
   ./build/bin/tool minimal.bc

Frequently Asked Questions
--------------------------

- **Which alias analysis?** Start with SparrowAA or DyckAA; use AserPTA
  (``-analysis-mode=1-cfa``) for callsite sensitivity, or Sea-DSA for memory graphs.
- **C++ input?** Compile with ``clang++ -emit-llvm -c -g`` and analyze the bitcode.
- **Multiple source files?** Compile each to bitcode, then combine them with
  ``llvm-link file1.bc file2.bc -o program.bc``.
- **Bitcode or text IR?** Use ``.bc`` for compact input, ``.ll`` for inspection.
- **Flow sensitivity?** Flow-sensitive analyses distinguish program points;
  flow-insensitive analyses combine information across a function or program.
- **Context sensitivity?** Distinguishes calls to the same function by their
  calling context, usually at additional analysis cost.
- **Which CLAM domain?** Start with ``int`` (intervals); try ``zones``, ``oct``,
  or ``pk`` when relational precision is needed. See :doc:`../tools/verifier/clam/index`.
- **How reliable are findings?** Review engine assumptions, unsupported features,
  and resource limits. Dynamic validation only covers executed paths.
- **How do I extend Lotus?** See :doc:`../developer/developer_guide` and
  :doc:`../developer/api_reference`.

Getting Help
------------

Check :doc:`installation`, :doc:`quickstart`, :doc:`tutorials`, and
:doc:`../tools/index` for setup and tool usage.
Report bugs at https://github.com/ZJU-PL/lotus/issues with:

- Lotus commit (``git rev-parse HEAD``), LLVM version (``llvm-config --version``),
  OS, and compiler version;
- a minimal reproducer, exact command, and complete error output;
- a debugger backtrace for crashes.

Use https://github.com/ZJU-PL/lotus/discussions for general questions.
