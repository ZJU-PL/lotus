<p align="center">
  <img src="docs/logo.jpg" alt="Lotus Logo" width="100"/>
</p>

# Lotus 

Lotus is a program analysis, verification, and optimization framework. It provides several toolkits that can be used individually or in combination to perform different tasks.
The current version has been tested on x86 Linux and ARM Mac using LLVM-12 and LLVM-14 with Z3-4.11.

## Major Components

### Alias Analysis

- **AllocAA**: Alias analsyis with simple heuristics.
- **DyckAA**: A unification-based, exhaustive alias analysis (See `lib/Alias/DyckAA`)
- **CFLAA**: All files in the subfolder are 1:1 copied from LLVM 14.0.6 and are subject to the LLVM license.
  We copy these files as LLVM removed them in the transition from version 14 to 15
- **Sea-DSA**: A context-sensitive, field-sensitive alias analysis based on Data Structure Analysis (See `lib/Alias/seadsa`) (It requires boost)
- **Andersen**: Context-insensitive points-to analysis implementation (without on-the-fly callgraph construction) (See `lib/Alias/Andersen`)
- **FPA**: Function Pointer Analysis with multiple approaches (FLTA, MLTA, MLTADF, KELP) for resolving indirect function calls (See `lib/Alias/FPA`)
- **DynAA**: Dynamic Alias Analysis Checker that validates static alias analysis results by observing pointer addresses at runtime (See `tools/dynaa`)
- **OriginAA**: K-callsite sensitve and origin-sensitive (from https://github.com/bozhen-liu/AFG)

### Intermediate Representations

- **PDG**: Program Dependence Graph
- **SSI**: Static Single Information (TBD)
- **DyckVFG**: See `lib/Alias/DyckAA/DyckVFG.cpp`

### Constraint Solving

- **SMT Solving**: Z3 integration (See `lib/Solvers/SMT`)
- **Binary Decision Diagram (BDD)**: CUDD-based implementation (See `lib/Solvers/CUDD`)
- **WPDS**: Weighted Pushdown System library (See `lib/Solvers/WPDS`)


### Abstract Interpretation

- **CLAM**: Abstract interpretation-based static analysis tool with multiple abstract domains for verification and bug detection (See `tools/clam` and `lib/clam`)
- **Sparta**: A header-only library for building abstract interpreters (See `include/Analysis/sparta`) (NOTE: this modules requies C++17 and boost)

### Utilities

- **cJSON**: A lightweight JSON parser/generator for C with a simple API (See `include/Support/cJSON.h`)
- **Transform**: Tranformations for LLVM bitcode

## Binary Tools

For detailed documentation on using the binary tools, see [TOOLS.md](TOOLS.md).

## Installation

### Prerequisites

- LLVM 12.0.0 or 14.0.0
- Z3 4.11
- CMake 3.10+
- C++14 compatible compiler

### Build LLVM

```bash
# Clone LLVM repository
git clone https://github.com/llvm/llvm-project.git
cd llvm-project

# Checkout desired version
git checkout llvmorg-14.0.0  # or llvmorg-12.0.0

# Build LLVM
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ../llvm
make -j$(nproc)  # Uses all available CPU cores
```

### Build Lotus

```bash
git clone https://github.com/ZJU-Automated-Reasoning-Group/lotus
cd lotus
mkdir build && cd build
cmake ../ -DLLVM_BUILD_PATH=/path/to/llvm/build
make -j$(nproc)
```

**Note**: The build system currently assumes that the system has the correct version of Z3 installed.

The build system will automatically download and build Boost if it's not found on your system. You can specify a custom Boost installation path with `-DCUSTOM_BOOST_ROOT=/path/to/boost`.

> **TODO**: Implement automatic download of LLVM and Z3 dependencies

## Publications

If you use Lotus in your research or work, please cite the following:

```bibtex
@misc{lotus2025,
  title = {Lotus: A Versatile and Industrial-Scale Program Analysis Framework},
  author = {Yao, Peisen},
  year = {2025},
  url = {https://github.com/ZJU-Automated-Reasoning-Group/lotus},
  note = {Program analysis framework built on LLVM}
}
```

### Papers that use Lotus

- **ISSTA 2025**: Program Analysis Combining Generalized Bit-Level and Word-Level Abstractions.  
  Guangsheng Fan, Liqian Chen, Banghu Yin, Wenyu Zhang, Peisen Yao, and Ji Wang.  
  *The ACM SIGSOFT International Symposium on Software Testing and Analysis.*

- **TSE 2024**: Fast and Precise Static Null Exception Analysis with Synergistic Preprocessing.  
  Yi Sun, Chengpeng Wang, Gang Fan, Qingkai Shi, Xiangyu Zhang.  
  *The IEEE Transactions on Software Engineering.*
