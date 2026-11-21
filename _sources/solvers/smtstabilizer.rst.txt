SMTStabilizer: SMT-LIB2 Normalization Library
=============================================

This section documents the SMTStabilizer library, which normalizes SMT-LIB2
inputs to reduce runtime variance caused by syntactic mutations.

Overview
--------

**Location**: ``include/Solvers/SMT/SMTStabilizer/`` (headers),
``lib/Solvers/SMT/SMTStabilizer/`` (implementation)

**Build target**: ``LotusSMTStabilizer`` (CMake alias ``Lotus::SMTStabilizer``)

**Build option**: ``-DLOTUS_ENABLE_SMT_STABILIZER=ON`` (requires GMP, GMPXX,
and MPFR)

SMTStabilizer normalizes SMT-LIB2 inputs so that syntactically different but
semantically equivalent encodings produce the same solver behavior. It targets
variance caused by assertion reordering, symbol renaming, and commutative
operand reordering. The component is integrated into Lotus and adapted to
compile as C++17; the original project is available at
<https://github.com/shaowei-cai-group/SMTStabilizer>.

The library is the engine behind the ``lotus-solver-smt-stabilizer``
command-line frontend; see :doc:`../tools/solver/index` for the tool.

Pipeline
--------

The stabilization pipeline runs parser, node manager, and kernel stages:

1. **Parser** — parse the SMT-LIB2 input into a DAG of typed nodes.
2. **Node manager** — simplify assertions and expose the assertion DAG.
3. **Kernel** — canonicalize symbols and reorder declarations and assertions.
4. **Serialization** — emit the normalized SMT2 text.

Modules
-------

api
~~~

Public API headers under ``include/Solvers/SMT/SMTStabilizer/api/``:

- ``stabilizer_api.h`` — ``stabilizer::api::SMTStabilizer`` facade with
  ``apply_file`` and ``apply_text``, plus ``SMTStabilizerOptions`` controlling
  rewrite normalization, context propagation, and subgraph pruning.
- ``stabilizer_c_api.h`` — C API with ``stabilizer_apply_file``,
  ``stabilizer_apply_text``, and ``stabilizer_free_string``.

parser
~~~~~~

SMT-LIB2 parser (based on the SOMTParser project) under
``include/Solvers/SMT/SMTStabilizer/parser/``:

- ``Parser`` — tokenizes and parses SMT-LIB2 commands and expressions.
- ``DAGNode`` / ``NodeManager`` — typed DAG representation with structural
  hashing and equivalence checking.
- ``Sort`` / ``SortManager`` — sort representation and deduplication.
- ``NODE_KIND`` — the operator and term kind enumeration.
- ``GlobalOptions`` — parser configuration (logic, precision, rewrite, etc.).
- ``Number`` / ``Integer`` / ``Real`` — GMP/MPFR-backed numeric types.
- ``Interval`` — interval arithmetic used in evaluation.
- ``Value`` — tagged value storage for constants.
- ``TypeChecker``, ``MathUtils``, ``BitVectorUtils``, ``StringUtils``,
  ``FloatingPointUtils``, ``ConversionUtils`` — helper utilities.

node
~~~~

``NodeManager`` (``stabilizer::node``) wraps the parser's assertion DAG and
provides ``simplify_assertions`` plus accessors used by the kernel.

kernel
~~~~~~

``Kernel`` (``stabilizer::kernel``) is the canonicalization engine. It builds
graph-oriented views over the assertion DAG and applies multi-stage hash
propagation to derive a deterministic symbol and function ordering. Its
``apply`` method renames symbols and uninterpreted-function declarations to
canonical names, reorders assertions and declaration blocks, and updates
datatype and sort naming maps. Context propagation and symmetry-breaking
perturbation are optional stages.

util
~~~~

Support utilities under ``include/Solvers/SMT/SMTStabilizer/util/``:

- ``BitVector`` — GMP-backed bit-vector arithmetic (from Bitwuzla).
- ``hash_node`` / ``hash_combine`` / ``hash_children`` — structural hashing
  helpers used by the kernel.
- ``gmp_utils`` — 64-bit-safe GMP wrappers.

Usage
-----

.. code-block:: cpp

   #include "Solvers/SMT/SMTStabilizer/api/stabilizer_api.h"

   stabilizer::api::SMTStabilizerOptions options;
   options.set_rewrite(true);
   options.set_context_propagation(true);
   options.set_subgraph_pruning(true);

   stabilizer::api::SMTStabilizer stabilizer(options);
   std::string normalized = stabilizer.apply_file("input.smt2");

License
-------

SMTStabilizer is distributed under the MIT License. The parser is based on the
SOMTParser project and was modified by the SMTStabilizer authors.

See Also
--------

- :doc:`../tools/solver/index` — the ``lotus-solver-smt-stabilizer``
  command-line frontend
- :doc:`smt` — core SMT solver infrastructure