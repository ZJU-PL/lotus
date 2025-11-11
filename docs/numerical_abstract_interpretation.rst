Numerical Abstract Interpretation (CLAM)
=========================================

CLAM provides abstract interpretation-based static analysis using numerical abstract domains.

Overview
--------

CLAM computes program invariants and verifies properties using abstract domains.

**Location**: ``tools/verifier/clam``, ``lib/Apps/clam``

**Features**: Multiple domains (intervals, zones, octagons, polyhedra), memory safety checking, Sea-DSA integration.

Abstract Domains
----------------

**Interval (int)**: Value ranges ``[l, u]`` - fast, simple ranges
**Zones**: Difference constraints ``x - y ≤ c`` - relational analysis
**Octagon (oct)**: Diagonal constraints ``±x ± y ≤ c`` - more precise
**Polyhedra (pk)**: Convex polyhedra ``∑ a_i x_i ≤ c`` - most precise

Usage
-----

.. code-block:: bash

   ./build/bin/clam [options] <input.bc>

**Domains**: ``--crab-dom=int|zones|oct|pk``
**Checks**: ``--crab-check=null|bounds|uaf|assert|all``
**Interprocedural**: ``--crab-inter``

Preprocessing (clam-pp)
------------------------

Prepare bitcode for analysis:

.. code-block:: bash

   ./build/bin/clam-pp --crab-lower-unsigned-icmp --crab-lower-select input.bc -o output.bc

**Options**: ``--crab-devirt``, ``--crab-externalize-addr-taken-funcs``

Differential Analysis (clam-diff)
---------------------------------

Compare analysis results:

.. code-block:: bash

   ./build/bin/clam-diff baseline.json modified.json

Workflow
--------

1. Preprocess: ``clam-pp input.bc -o prep.bc``
2. Analyze: ``clam --crab-dom=zones --crab-check=assert prep.bc``
3. Export: ``clam -ojson=results.json prep.bc``

