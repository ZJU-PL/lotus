CLAM – Numerical Abstract Interpretation
=========================================

CLAM is a static analysis framework based on abstract interpretation over
numerical abstract domains. It is designed for inferring invariants and
checking safety properties over LLVM bitcode.

**Location**: ``tools/verifier/clam/``

Overview
--------

CLAM computes sound over-approximations of program behaviors by interpreting
the program over different numerical domains.

**Headers**: ``include/Apps/clam``

**Implementation**: ``lib/Apps/clam`` (core library), ``tools/verifier/clam`` (command-line frontends)

**Key features**:

- Multiple abstract domains: intervals, zones, octagons, polyhedra
- Memory safety checking (null, bounds, use-after-free)
- Assertion checking
- Integration with Sea-DSA for precise memory modeling

Internally, CLAM builds on the Crab numerical abstract interpretation library
and Lotus analysis infrastructure (CFG utilities, alias analysis, and memory
modeling) exposed under ``include/Analysis`` and related modules.

Abstract Domains
----------------

Choose a domain that balances precision and performance for your workload:

- **Interval (int)**:

  - Value ranges ``[l, u]``
  - Fastest and most scalable
  - Good for coarse invariants and quick checks

- **Zones**:

  - Difference constraints ``x - y ≤ c``
  - Captures simple relational properties between variables

- **Octagon (oct)**:

  - Diagonal constraints ``±x ± y ≤ c``
  - More precise than zones with moderate overhead

- **Polyhedra (pk)**:

  - Convex polyhedra ``∑ a_i x_i ≤ c``
  - Highest precision, highest cost
  - Suitable for small, critical kernels

Command-Line Usage
------------------

Invoke CLAM on LLVM bitcode:

.. code-block:: bash

   ./build/bin/clam [options] <input.bc>

Common options:

- **Domains**: ``--crab-dom=int|zones|oct|pk``
- **Checks**: ``--crab-check=null|bounds|uaf|assert|all``
- **Interprocedural**: ``--crab-inter``
- **Output**: ``--crab-out=results.json`` (JSON output)

Preprocessing (clam-pp)
-----------------------

Use ``clam-pp`` to normalize and simplify bitcode before analysis:

.. code-block:: bash

   ./build/bin/clam-pp \
      --crab-lower-unsigned-icmp \
      --crab-lower-select \
      input.bc -o output.bc

Useful options:

- ``--crab-devirt`` – Devirtualize indirect calls when possible
- ``--crab-externalize-addr-taken-funcs`` – Externalize address-taken functions

Differential Analysis (clam-diff)
---------------------------------

Use ``clam-diff`` to compare analysis results across versions:

.. code-block:: bash

   ./build/bin/clam-diff baseline.json modified.json

Typical Workflow
----------------

1. **Preprocess**:

   .. code-block:: bash

      ./build/bin/clam-pp input.bc -o prep.bc

2. **Analyze**:

   .. code-block:: bash

      ./build/bin/clam \
         --crab-dom=zones \
         --crab-check=assert \
         prep.bc

3. **Export results** (optional):

   .. code-block:: bash

      ./build/bin/clam \
         --crab-dom=zones \
         --crab-check=assert \
         --crab-out=results.json \
         prep.bc

