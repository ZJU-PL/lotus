Mosaic
======

The **Mosaic CHC Verifier** provides facilities for Constrained Horn Clause (CHC) 
verification with advanced preprocessing and translation techniques.

**Location**: ``include/Verification/Mosaic/``, ``lib/Verification/Mosaic/``

**CLI Tool**: ``tools/verifier/mosaic/`` (binary ``lotus-verify-mosaic``)

Overview
--------

Mosaic acts as a verification backend leveraging Z3's fixed-point engine. 
Its primary feature is the ability to translate between Bit-Vector (BV) and 
Integer (Int) arithmetic domains before and during CHC solving, which is 
critical for performance and decidability in many software verification tasks.

Key Components
--------------

* **MosaicFixedpoint**: The core CHC verification engine wrapping Z3's 
  fixed-point solver with custom CHC rule management and solving strategies.
* **Bv2IntTranslator** / **Int2BvTranslator**: Utilities for translating 
  constraints from bit-vector arithmetic to integer arithmetic, and vice versa. 
  This allows the solver to pick the most efficient background theory for the 
  CHC problem at hand.
* **Int2BvPreprocessor** / **Int2BvOverflowAnalyzer**: Analyzes whether integer 
  constraints can be safely encoded into finite-width bit-vectors by checking 
  for potential overflows, applying simplifications and reducing the state space.
* **CHCParser**: Parses external CHC problems for verification.

Usage
-----

Mosaic is built into the ``lotus-verify-mosaic`` tool:

.. code-block:: bash

   ./build/bin/lotus-verify-mosaic input.smt2

It integrates closely with Lotus's analysis infrastructure to resolve constraints 
extracted from LLVM IR.
