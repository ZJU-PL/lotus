# CLAM Integration in Lotus

This directory contains the CLAM (Abstract Interpretation-based Analyzer for LLVM bitcode) library integrated into Lotus.

## Overview

CLAM is an abstract interpretation framework that provides:
- Numerical abstract domains (intervals, octagons, polyhedra, etc.)
- Invariant generation for LLVM IR
- Property checking (null pointer, buffer overflow, etc.)
- Integration with sea-dsa for heap analysis

## Structure

- `*.cc` files: CLAM core implementation
- `crab/`: CRAB abstract domain library integration
- `Properties/`: Property checkers (null, bounds, UAF)
- `Support/`: Utility functions
- `Optimizer/`: Post-analysis optimizations

## Integration with Lotus

CLAM is integrated as a library that can be used by Lotus analyses:

1. **Wrapper API**: See `include/Analysis/Clam/ClamAnalysis.h` for the wrapper interface
2. **Usage Example**:
```cpp
#include "Analysis/Clam/ClamAnalysis.h"

using namespace lotus::clam;

// Create CLAM analysis
auto clam = createClamAnalysis(module);

// Run analysis
clam::AnalysisParams params;
clam->analyze(params);

// Query invariants
auto pre = clam->getPre(basicBlock);
if (pre.has_value()) {
  // Use invariants for more precise analysis
}
```

## Build Configuration

CLAM is enabled by default. To disable:
```bash
cmake -DENABLE_CLAM=OFF ..
```

## Dependencies

- **CRAB**: Abstract domain library (cloned as submodule)
- **sea-dsa**: Heap abstraction (already in Lotus)
- **Boost**: Required for CRAB
- **LLVM 14**: Same version as Lotus

## References

- CLAM GitHub: https://github.com/seahorn/clam
- CRAB GitHub: https://github.com/seahorn/crab
- Sea-DSA: https://github.com/seahorn/sea-dsa

