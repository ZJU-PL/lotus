Sprattus – Symbolic Abstraction Framework
==========================================

**Location**: ``tools/verifier/sprattus/``

**Library**: ``lib/Analysis/Sprattus``

**Headers**: ``include/Analysis/Sprattus``

Overview
--------

Sprattus is a framework for static program analysis of low-level C
and C++ programs on LLVM IR. It uses **Symbolic Abstraction** to provide a flexible interface for designing program analyses
in a compositional way.

With Sprattus you describe the semantic properties you care about, not the
effect of every LLVM instruction. The framework then derives sound static
analyses that respect those semantics.

Sprattus combines:

- **Abstract Interpretation**: Approximates program semantics using abstract domains
- **Fragment-based Analysis**: Decomposes functions into acyclic subgraphs for scalability
- **SMT Integration**: Uses Z3 for precise constraint solving
- **Composable Domains**: Combine multiple abstract domains for rich analyses

Key Features
------------

- **Multiple Abstract Domains**: NumRels, Intervals, BitMask, Affine, Predicates, SimpleConstProp, Boolean, MemRange, MemRegions
- **Flexible Fragment Strategies**: Control where abstraction points appear
- **LLVM Integration**: Works seamlessly with LLVM 12/14
- **Z3-backed**: Leverages SMT solving for precision

Installation
------------

Sprattus is integrated into Lotus. After building Lotus:

.. code-block:: shell

   cd build
   make SprattusAnalysis    # Build the library
   make sprattus            # Build the analysis tool

The tool will be located at ``build/bin/sprattus``.

Quick Start
-----------

Example Program
~~~~~~~~~~~~~~~

Consider this simple C program (``example.c``):

.. code-block:: c

   #include <stdint.h>

   uint64_t foo(uint8_t a)
   {
       uint64_t x = a;
       x = x | (x << 32);
       x = x | (x << 16) | (x << 48);
       return x;
   }

   int main(int argc, char** argv)
   {
       uint64_t res = foo(42);
       return res > 0;
   }

Compiling to LLVM Bitcode
~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: shell

   # Compile to bitcode
   clang -O2 -c -g -emit-llvm -mllvm -inline-threshold=0 -o out.bc example.c

   # Convert to SSA form and name instructions
   opt -mem2reg -instnamer out.bc -o example.bc

Running the Analyzer
~~~~~~~~~~~~~~~~~~~~

.. code-block:: shell

   # Analyze all functions
   ./bin/sprattus example.bc

   # Analyze specific function
   ./bin/sprattus --function=foo example.bc

   # With verbose output
   ./bin/sprattus --function=foo --verbose example.bc

   # List available functions
   ./bin/sprattus --list-functions example.bc

Command-Line Options
--------------------

Basic Options
~~~~~~~~~~~~~

``--function=<name>``
  Analyze a specific function by name

``--list-functions``
  List all functions in the module

``--verbose``
  Enable verbose output showing detailed analysis information

``--help``
  Display available options

Abstract Domain Options
~~~~~~~~~~~~~~~~~~~~~~~

``--list-domains``
  List the abstract domains that were registered in this build

``--abstract-domain=<name>``
  Run the analysis with the named abstract domain (see ``--list-domains``)

Input Requirements
------------------

Sprattus expects LLVM bitcode files that are:

1. **In SSA form**: Run ``opt -mem2reg`` to convert
2. **With named instructions**: Run ``opt -instnamer`` for readable output
3. **Optimized** (optional): Use ``clang -O2`` for better results

Fragment Strategies
-------------------

Fragment decomposition controls where analysis maintains abstract states.
The strategy determines the trade-off between precision and performance.

Available Strategies
~~~~~~~~~~~~~~~~~~~~

``Edges``
  - Abstracts after every basic block
  - Most precise, slowest
  - Good for small functions

``Function``
  - Analyzes whole function as one fragment
  - Fastest, least precise
  - Good for very large functions

``Headers``
  - Places abstraction points at loop headers
  - Derives loop invariants
  - Good balance for loops

``Body``
  - Abstracts in loop bodies
  - Alternative to Headers

``Backedges``
  - Abstracts at loop backedges
  - Another loop handling strategy

Abstract Domains
----------------

Sprattus provides multiple abstract domains that can be combined:

Numeric Domains
~~~~~~~~~~~~~~~

**NumRels** (Numerical Relations)
  Tracks relationships between integer variables (e.g., ``x <= y + 5``)

**Intervals**
  Maintains value ranges for integers (e.g., ``x ∈ [0, 100]``)

**Affine**
  Tracks affine relationships (e.g., ``y = 2*x + 3``)

**SimpleConstProp**
  Simple constant propagation

Bit-Level Domains
~~~~~~~~~~~~~~~~~

**BitMask**
  Tracks known bits and alignment (e.g., ``x & 0x7 == 0`` means 8-byte aligned)

Boolean Domains
~~~~~~~~~~~~~~~

**Boolean**
  Represents truth values and boolean invariants

**Predicates**
  Path predicates and assertions

Memory Domains
~~~~~~~~~~~~~~

**MemRange**
  Describes memory access bounds in terms of function arguments

**MemRegions**
  Analyzes memory regions and pointer relationships

Advanced Usage
--------------

Customizing Analysis
~~~~~~~~~~~~~~~~~~~~

To customize the analysis, modify ``tools/verifier/sprattus/spranalyze.cpp``:

.. code-block:: cpp

   // Change fragment strategy
   auto fragments = sprattus::FragmentDecomposition::For(fctx, 
       sprattus::FragmentDecomposition::Headers);

   // Configure specific domains (requires code changes)
   // See DomainConstructor class for available domains

Integration as LLVM Pass
~~~~~~~~~~~~~~~~~~~~~~~~~

Sprattus can be used as an LLVM pass via ``SprattusPass``:

.. code-block:: cpp

   #include "Analysis/Sprattus/SprattusPass.h"

   // Use in your pass manager
   // The pass performs analysis and can be configured programmatically

Examples
--------

Example 1: Array Bounds Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   void process_array(int* arr, int size) {
       for (int i = 0; i < size; i++) {
           arr[i] = i * 2;
       }
   }

Sprattus can verify that array accesses are within bounds if ``size`` is known
or bounded.

Example 2: Bit Pattern Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   uint64_t replicate_byte(uint8_t b) {
       uint64_t x = b;
       x |= (x << 8);
       x |= (x << 16);
       x |= (x << 32);
       return x;
   }

Sprattus's BitMask domain can track how bits propagate through shifts and
bitwise operations.

Example 3: Pointer Arithmetic
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   int* find_element(int* base, int offset) {
       return base + offset;
   }

MemRange domain can express the result in terms of input parameters.

Test Cases
----------

Several test cases are available in ``tests/sprattus/``:

.. code-block:: shell

   # Simple assertion test
   cd tests/sprattus/assertions
   clang -c -emit-llvm simple_assert.c -o simple_assert.bc
   opt -mem2reg -instnamer simple_assert.bc -o simple_assert_ssa.bc
   ../../build/bin/sprattus simple_assert_ssa.bc

Limitations
-----------

Current Limitations
~~~~~~~~~~~~~~~~~~~

1. **No Python Configuration**: Original Sprattus used Python config files.
   This integration requires C++ code changes for advanced configuration.

2. **No Dynamic Analysis**: Runtime instrumentation support was removed.

3. **Simplified Type Checking**: Some RTTI-dependent features simplified
   to work with LLVM's ``-fno-rtti`` requirement.

4. **Debug Info**: Source-level variable names not always available.

Performance Considerations
~~~~~~~~~~~~~~~~~~~~~~~~~~

- Fragment strategy significantly impacts analysis time
- Start with ``Function`` strategy for large codebases
- Use ``Edges`` or ``Headers`` for precision-critical code
- SMT queries can be expensive for complex constraints

Troubleshooting
---------------

Common Issues
~~~~~~~~~~~~~

**"Function not in SSA form"**
  Run ``opt -mem2reg`` on your bitcode

**"Function not found"**
  Use ``--list-functions`` to see available functions
  Check function name mangling for C++

**Slow analysis**
  Try a coarser fragment strategy
  Reduce the number of active domains

**Unexpected results**
  Enable ``--verbose`` to see detailed analysis steps
  Check if input assumptions are violated

Technical Details
-----------------

Fragment-Based Analysis
~~~~~~~~~~~~~~~~~~~~~~~

Sprattus decomposes each function's control flow graph into acyclic
fragments. Each fragment is analyzed separately, with abstraction points
at fragment boundaries maintaining the abstract state.

This approach provides:

- **Scalability**: Large functions analyzed incrementally
- **Precision**: Fine-grained control over abstraction points
- **Compositionality**: Results combine naturally

Symbolic Abstraction
~~~~~~~~~~~~~~~~~~~~

The key insight is to encode program semantics and abstract domains
as SMT constraints. The analyzer:

1. Converts LLVM instructions to Z3 expressions
2. Queries abstract domains to refine the state
3. Uses SMT solver to check satisfiability
4. Iterates to fixed point

This provides both automation and precision.

Implementation Notes
~~~~~~~~~~~~~~~~~~~~

- **LLVM Version**: Integrated with LLVM 12/14
- **SMT Solver**: Z3 4.x or later required
- **Build System**: CMake integration with Lotus
- **Language**: C++14

References
----------

For more information:

- Lotus documentation: ``README.md``
- Sprattus integration notes: ``SPRATTUS_INTEGRATION_SUCCESS.md``
- Original implementation: ``~/Work/sprattus``

Contributing
------------

To extend Sprattus:

1. Add new abstract domains in ``lib/Analysis/Sprattus/domains/``
2. Register domains in ``DomainConstructor.cpp``
3. Update documentation
4. Add test cases

Future Directions
-----------------

Potential enhancements:

- Configuration file support (non-Python)
- More abstract domains (octagons, polyhedra)
- Interprocedural analysis
- Parallel fragment analysis
- JSON output for tool integration
- Source code annotations

Support
-------

For questions or issues:

- Check ``SPRATTUS_INTEGRATION_SUCCESS.md`` for known limitations
- Review test cases in ``tests/sprattus/``
- Examine source code in ``lib/Analysis/Sprattus/``

Conclusion
----------

Sprattus provides powerful static analysis capabilities through symbolic
abstraction. While some features from the original implementation were
simplified or removed for LLVM 12/14 compatibility, the core analysis
engine remains fully functional and can analyze complex C/C++ programs.
