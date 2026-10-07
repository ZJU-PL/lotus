SMT Samplers
============

This section documents the SMT sampling utilities in Lotus for extracting
diverse satisfying assignments (models) from SMT formulas.

Overview
--------

The ``SMTSampler`` library provides multiple algorithms for sampling from the
solution space of SMT formulas. These samplers are useful for test case generation,
solution space exploration, and analyzing formula sensitivity.

**Location**: ``lib/Solvers/SMT/SMTSampler/``

**Library**: ``SMTSampler``

**Dependencies**:
- SymAbs (symbolic abstraction library)
- Z3 SMT solver

Samplers
--------

QuickSampler
~~~~~~~~~~~~

A mutation-based approach for generating diverse models from CNF formulas.

**Input**: CNF in DIMACS format with optional ``c ind`` lines for independent variables

**Output**: ``<input>.samples`` - one line per sample in format ``<mutations>: <bitstring>``

**Algorithm**: Uses genetic mutation operators to explore the solution space, stopping
on time or sample limits.

**Related Work**: Based on ICSE 18 paper "Efficient Sampling of SAT Solutions for Testing"

IntervalSampler
~~~~~~~~~~~~~~~

Computes per-variable bounds using Z3's optimize commands and samples uniformly within
those bounds.

**Input**: SMT-LIB bit-vector formulas (single file or directory)

**Output**: ``res.log`` (append-only summary statistics)

**Algorithm**:
1. Use Z3 optimize to find variable bounds
2. Sample uniformly from computed intervals
3. Validate samples against original formula

RegionSampler (PolySampler)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Geometry-based sampling from convex polytopes defined by linear constraints.
Uses the SymAbs library to build linear abstractions (Zone or Octagon domains).

**Input**: SMT-LIB bit-vector formulas

**Output**: ``<input>.abs.samples`` with first line as variable name header

**Algorithm**:
1. Build linear abstraction (Zone or Octagon) from formula
2. Use hit-and-run (or other) random walk to propose samples
3. Validate proposed samples against original formula

**Available Walks**:
- ``HitAndRun`` - default, geometric random walk
- ``BallWalk`` - ball-based random walk
- ``DikinWalk`` - Dikin walk for log-concave distributions
- ``CoordinateWalk`` - coordinate axis random walk
- ``ConstraintWalk`` - constraint-based random walk

C++ API Usage
-------------

The samplers are invoked through functions declared in ``Solvers/SMT/SMTSampler/SMTSampler.h``:

.. code-block:: cpp

   #include "Solvers/SMT/SMTSampler/SMTSampler.h"

   // Run mutation-based sampling on a DIMACS CNF formula;
   // writes samples to <input>.samples
   lotus::SMTSampler::runQuickSampler("formula.cnf", 100, 60.0);

   // Run interval-based sampling on an SMT-LIB file or directory;
   // appends summary statistics to res.log
   lotus::SMTSampler::runIntervalSampler("problem.smt2", 1000, 30000.0);

   // Run region-based random-walk sampling on an SMT-LIB file;
   // writes samples to <input>.abs.samples
   lotus::SMTSampler::runRegionSampler(
       "problem.smt2",
       500,
       30000.0,
       lotus::SMTSampler::Domain::OCTAGON,
       lotus::SMTSampler::Walk::HIT_AND_RUN);

**QuickSampler Output Format** (in ``<input>.samples``)::

   3: 1010110101
   5: 0011101010
   ...

Each line contains ``<number_of_mutations>: <bitstring>``.

Related Work
------------

- FMCAD 19: GUIDEDSAMPLER - Coverage-guided Sampling of SMT Solutions
- ICSE 18: Efficient Sampling of SAT Solutions for Testing

See Also
--------

- :doc:`smt` - SMT solver backend
- :doc:`symabs` - Symbolic abstraction library (used by RegionSampler)
