Solver Tools
============

This page documents the command-line front-ends under ``tools/solver/``.
``lotus-solver-datalog`` is built by default, while ``lotus-solver-owl`` requires
``-DLOTUS_ENABLE_OWL=ON``. ``lotus-solver-staub`` remains a source-present
experimental tool; ``lotus-solver-smt-stabilizer`` requires
``-DLOTUS_ENABLE_SMT_STABILIZER=ON`` (GMP/MPFR). SMT↔LLVM translation is provided
by TUNA under ``lib/Solvers/SMT/TUNA``.

lotus-solver-datalog – Datalog Solver Front-End
-----------------------------------------------

The native Datalog/lattice solver front-end accepts JSON Semantic IR, Lotus
Datalog, and Z3 fixedpoint input. It validates or executes programs and emits
canonical JSON relation rows and runtime statistics.

**Binary**: ``lotus-solver-datalog``

**Source**: ``tools/solver/datalog/``

.. code-block:: bash

   ./build/bin/lotus-solver-datalog schema > program.json
   ./build/bin/lotus-solver-datalog validate program.json
   ./build/bin/lotus-solver-datalog run program.json --workers 4 --pretty

lotus-solver-owl – SMT/Model Checking Front-End
-----------------------------------------------

``lotus-solver-owl`` is the supported solver front-end currently built from this
directory. It feeds SAT or SMT problems to the configured solver stack.

**Binary**: ``lotus-solver-owl``
**Location**: ``tools/solver/lotus-solver-owl.cpp``

**Build status**: built only when ``-DLOTUS_ENABLE_OWL=ON``.

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-solver-owl file.smt2

**Example**:

.. code-block:: bash

   ./build/bin/lotus-solver-owl examples/solver/example.smt2

See :doc:`../../solvers/smt` for details about the solver stack.

lotus-solver-staub – Bounded-Theory Conversion Front-End
--------------------------------------------------------

``lotus-solver-staub`` rewrites unbounded SMT constraints into bounded encodings
before translation or solving.

**Binary**: ``lotus-solver-staub`` (source present, not built by default)

**Source**: ``tools/solver/lotus-solver-staub.cpp``

This front-end is kept in the tree as an experimental source tool, not as a
default-built binary.

Basic usage:

.. code-block:: bash

   ./build/bin/lotus-solver-staub -s query.smt2 -i aix -o bounded.smt2
   ./build/bin/lotus-solver-staub -s query.smt2 -r 8,24 -o bounded.smt2

Important options:

- ``-s <file>`` – input SMT-LIB2 file
- ``-o <file>`` – output transformed formula
- ``-t <file>`` – write statistics
- ``-l`` – emit output compatible with SLOT
- ``-i <N|aix|aix2>`` – integer bounding mode
- ``-r <ebits,sbits|aix|aix4>`` – floating-point bounding mode

lotus-solver-smt-stabilizer – SMT Normalization Front-End
---------------------------------------------------------

``lotus-solver-smt-stabilizer`` normalizes SMT-LIB2 inputs to reduce runtime
variance caused by syntactic mutations such as assertion reordering, symbol
renaming, and commutative operand reordering.

**Binary**: ``lotus-solver-smt-stabilizer`` (requires ``-DLOTUS_ENABLE_SMT_STABILIZER=ON``)

**Source**: ``tools/solver/lotus-solver-smt-stabilizer.cpp``

Enable it when configuring Lotus (needs GMP, GMPXX, and MPFR):

.. code-block:: bash

   cmake -S . -B build -DLOTUS_ENABLE_SMT_STABILIZER=ON
   cmake --build build --target LotusSMTStabilizer lotus-solver-smt-stabilizer
   ./build/bin/lotus-solver-smt-stabilizer query.smt2 > normalized.smt2

Important options:

- ``<file>`` – input SMT-LIB2 file (reads stdin when omitted)
- ``--no-cp`` – disable context propagation
- ``--no-sbp`` – disable symmetry-breaking perturbation
