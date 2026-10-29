<p align="center">
  <img src="docs/source/_static/logo.svg" alt="Lotus — LLVM Static Analysis Framework Logo" width="120"/>
</p>

# Lotus: LLVM-based Static Analysis Framework

[![Documentation](https://img.shields.io/badge/docs-zju--pl.github.io-blue.svg)](https://zju-pl.github.io/lotus)
[![DeepWiki Docs](https://img.shields.io/badge/deepwiki-ZJU--PL%2Flotus-blueviolet)](https://deepwiki.com/ZJU-PL/lotus)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![C++](https://img.shields.io/badge/c++-14%2F17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![LLVM](https://img.shields.io/badge/LLVM-14.x-purple.svg)](https://llvm.org/)

**Lotus** is an LLVM-based program analysis and verification framework. It provides a comprehensive set of toolkits for alias analysis, dataflow analysis, concurrency analysis, abstract interpretation, and model checking. The framework is designed for high modularity, allowing components to be used independently or in combination.

## Features

- **Alias Analysis** — Pointer analysis with flow-sensitive, context-sensitive, and context-insensitive variants (e.g., AserPTA, DyckAA, LotusAA, SeaDSA).
- **Dataflow Analysis** — Distributive (IFDS/IDE), Monotone, Elimination-based (APA), Weighted Pushdown Systems (WPDS), etc.
- **Intermediate Representations** — Extends LLVM with specialized IRs like ICFG, PDG, SVFG (with sparse MemorySSA), SSI, and GSA.
- **Bug Detection** — Detects memory safety issues, concurrency bugs, integer overflows, taint-style issues, and typestate violations using engines like Kint, AE, Pulse, and Saber.
- **Formal Verification** — Abstract interpretation using CLAM, SeaHorn, and custom symbolic execution backends.
- **Program Optimization** — Partial evaluation, prefetching, etc.

## Quick Start

### System Requirements
- **OS**: x86/ARM Linux, ARM macOS
- **Compiler**: C++17 compatible (GCC 7+ or Clang 5+)
- **Dependencies**: LLVM 14.x, Z3 4.11, CMake 3.18+, (Optional: Boost 1.80+)

### Building from Source

Lotus uses a standard out-of-source CMake build.

```bash
git clone https://github.com/ZJU-PL/lotus
cd lotus
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

> If LLVM is installed in a custom location, append `-DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm` to your `cmake` command. See [INSTALL.md](INSTALL.md) for detailed configuration options.

### Running an Analysis

Lotus exposes its internal libraries through standalone executable drivers in `build/bin/`. All tools operate on LLVM bitcode (`.bc`) files.

```bash
# 1. Compile target program to LLVM bitcode
clang -emit-llvm -c example.c -o example.bc

# 2. Run a pointer analysis
./build/bin/lotus-alias-sparrow-aa example.bc

# 3. Run bug-detection
./build/bin/lotus-check --engine=saber example.bc
```

## Documentation

- **Official Docs**: [zju-pl.github.io/lotus](https://zju-pl.github.io/lotus)
  - [Architecture & Major Components](https://zju-pl.github.io/lotus/user_guide/architecture.html)
  - [Command-Line Tools Reference](https://zju-pl.github.io/lotus/tools/index.html)
  - [Getting Started / Quickstart](https://zju-pl.github.io/lotus/user_guide/quickstart.html)
- **DeepWiki Reference**: [deepwiki.com/ZJU-PL/lotus](https://deepwiki.com/ZJU-PL/lotus)

## Publications

If you use Lotus in your research or work, please cite the following:

```bibtex
@misc{lotus2025,
  title = {Lotus: A Versatile and Industrial-Scale Program Analysis Framework},
  author = {ZJU Programming Languages and Automated Reasoning Group},
  year = {2025},
  url = {https://github.com/ZJU-PL/lotus},
  note = {Program analysis framework built on LLVM}
}
```

**Papers utilizing Lotus frameworks:**
- **PPoPP 2027**: Cactus: GPU-Accelerated Alias Query Processing over Persistent Points-to Information. Zinan Gu, Zhaoyang Tan, Peisen Yao, and Kui Ren.
- **SPLASH/ISSTA 2026 Demo**: Phoenix: A Modular and Versatile Framework for C/C++ Pointer Analysis. Peisen Yao, Zinan Gu, and Qingkai Shi.
- **ASE 2026**: SIMD-Accelerated Sparse Bit-Vectors for Pointer Analysis. Zhaoyang Tan, Peisen Yao, and Kui Ren.
- **FM 2026**: EUF-based Solving Dyck-Reachability with Applications to Static Analysis. Yide Du, Zhenbang Chen, Kunlin Liu, Guofeng Zhang, Xudong Wang, Ke Ma, Wei Dong, and Ji Wang.
- **CAV 2026**: Sound and Precise Symbolic Automata Model for Stateful Software Systems. Xinlong Wu, Ruiyu Zhou, Peisen Yao, and Qingkai Shi.
- **TOSEM 2026**: Compiler Optimizations-Based SMT Simplifications: An In-Depth Study. Hanyun Jiang, Peisen Yao*, Jiachen Lu, Yongwang Zhao, and Kui Ren.
- **ISSTA 2025**: Program Analysis Combining Generalized Bit-Level and Word-Level Abstractions. Guangsheng Fan, Liqian Chen, Banghu Yin, Wenyu Zhang, Peisen Yao, and Ji Wang.
- **S&P 2024**: Titan: Efficient Multi-target Directed Greybox Fuzzing. Heqing Huang, Peisen Yao, Hung-Chun Chiu, Yiyuan Guo, and Charles Zhang.
- **USENIX Security 2024**: Unleashing the Power of Type-Based Call Graph Construction by Using Regional Pointer Information. Yuandao Cai, Yibo Jin, and Charles Zhang.
- **TSE 2024**: Fast and Precise Static Null Exception Analysis with Synergistic Preprocessing. Yi Sun, Chengpeng Wang, Gang Fan, Qingkai Shi, and Xiangyu Zhang.
- **OOPSLA 2022**: Indexing the Extended Dyck-CFL Reachability for Context-Sensitive Program Analysis. Qingkai Shi, Yongchao Wang, Peisen Yao, and Charles Zhang.

## Contributors

- rainoftime / cutelimination
- qingkaishi
- rhuab
- Zahrinas
- Rexxar-Jack-Remar
- x6eull
- hiruwaKS
