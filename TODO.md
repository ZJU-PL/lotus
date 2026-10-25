# Development

For Summer Research, Final Year Project Topics, etc.

## Frontend

- Go: https://github.com/goplus/llgo
- MLIR: https://github.com/seahorn/mlir2crab
- Btor2
- SystemVerilog
- Java
- Circom: https://github.com/whbjzzwjxq/ZKAP/tree/master/circom2llvm
- ...?
- Rust?: https://github.com/seahorn/verify-rust
  - FMCAD 22: Bounded Model Checking for LLVM
  - FMCAD 25: A Tale of Two Case Studies: A Unified Explorationof Rust Verification with SEABMC


## 1. Testing Improvements

- Different OS, LLVM, Z3, Boost, etc.
- Use lib/Alias/Dynamic to test pointer analyses.

## 2. Alias Analysis

### Infrastructure

- Specificatin: use lib/Annotation to enable the loading of aliasing specification files for the pointer analyses (And perhpas use dynamic analysis/ML to extract specifications)
- Data structure: integrate different representations of points-to sets.

### Interfaces

Universal interface for the analyses in lib/Alias: 

- Basics: points-to, alias pair, alias set, pointed-by, callgraph, memory dependence, etc.
- Pointer queries: points-to, alias pair, alias set, pointed-by set
- Callgraph: callgraph edges, reachable methods, etc.
- Optimizations: devirtualization, dead code elimination, ...
- Security: bug finding, guided fuzzing, control-flow integrity, code pointer integrity, ...

**Note**: Currently, we may not focus on some "high-level clients" such as taint analysis and memory safety verification, which can require more reasoning capabilities dataflow tracking, numerical analysis, path sensitivity, etc.

We have AliasWrapper.cpp that wraps various alias analyses (to be tested),


### Third-Party

* Integrate the pointer analyses in SVF and DG


## 3. Intermediate Representations (IR) 


### PDG (Program Dependence Graph)

* Use the pointer analysis interfaces (currently, it relies on the memory dependence analysis inside LLVM)


Implement other algorithms

- TSE 22: The Duality in Computing SSA Programs and Control Dependency
- SAS 22: Fast and Incremental Computation of Weak Control Closure
- TOPLAS 21: On Time-sensitive Control Dependencies

## 4. Applications 

### Bug Detection

- Buffer overflow detection?
- Memory leak detection?
- Race condition analysis?

Some related publications
- S&P 19:  RAZZER: Finding Kernel Race Bugs through Fuzzing,
- CCS 18:  Hawkeye: Towards a Desired Directed Grey-box Fuzzer

### Software Protection

**Side Channel**
- TOPLAS 23: Side-Channel Elimination via Partial Control-Flow Linearization. Luigi Soares, Michael Canesche, and Fernando Magno Quintão Pereira。
- Security 26: VeCT: Secure and Efficient Constant-Time Code Rewriting with Vector Extensions.
- CCS 21: Constantine: Automatic Side-Channel Resistance Using Efficient Control and Data Flow Linearization. https://github.com/pietroborrello/constantine

**Integriry?**
- CCS 22: C2C: Fine-Grained Configuration-Driven System Call Filtering
- USENIX Security 20: Temporal System Call Specialization for Attack Surface Reduction
- USENIX Security 19:  Origin-Sensitive Control Flow Integrity
- ISSTA 17:  Boosting the Precision of Virtual Call Integrity Protection with Partial Pointer Analysis for C++ 


## 5. Numeric
al Analysis

+ Crab/Lotus concurrent fixpoint performance:
  - Study speculative concurrency for Crab's `concurrenty_fwd_fixpoint_iterator`: allow bounded stale reads inside a WPO SCC, then repair deterministically at loop heads.
  - Use delta-based propagation for abstract states so workers transfer only changed facts, not full domains, across edges.
  - Add instability-aware scheduling for concurrent fixpoint: prioritize hot loop heads, high-fanout nodes, and large abstract-state deltas.
  - Explore a dual-lane design: conservative committed invariants plus speculative worker invariants, with commit only when speculation is safe.
  - Compare against Crab's sequential/interleaved fixpoint and measure speedup, extra rework, convergence iterations, and precision loss (if any).
+ Data structure optimization for abstract domains
  - Better environment maps for abstract interpretation.
    Crab still reflects the classic “Patricia tree as efficient mergeable map” design. A
    serious study comparing Patricia trees, flat_map, B-tree variants, HAMTs, and chunked
    persistent vectors under real analyzer workloads would be valuable. The key metric is
    not only asymptotic merge cost but allocator pressure and cache locality during
    fixpoint iteration.
  - Graph representations for DBM domains.
    Crab already exposes multiple graph backends, which is a sign the choice matters. A
    good research problem is adaptive switching between graph representations based on
    density, update pattern, or SCC phase. Another is incremental closure algorithms that
    exploit workload structure instead of rerunning generic sparse shortest-path
    machinery.

## 6. Dataflow Analysis

**Facilities**

The different engines have many clients that overlap.
We need to extarct some common facilites, e.g., the abstract domains, instead of keeping the independent, ad-hoc implementations under different engines.

**Monontone**
* Revise the monotone dataflow analysis module


**WPDS-based Datafow analysis (lib/Dataflow/WPDS)**

- How to design alias-aware WPDS analyses?
- We use third-party/WPDS for now, but there is an extended, C++ version in third-party/WALi-OpenNWA. Shoud we try to use the new one?
- Code ref.: https://github.com/icra-team/icra (it adds some extensions such as Newtonian Program Analysis?)


## Bindings

- Python (e.g., following https://github.com/SVF-tools/SVF-Python and Z3?)

## Investigate More Related Work

- [ccylzer](https://github.com/GaloisInc/cclyzerpp), Yannis's SAS 16.
- [DG](https://github.com/mchalupa/dg) - Dependence Graph for analysis of LLVM bitcode ([paper1](https://www.fi.muni.cz/~xchalup4/dg_atva20_preprint.pdf), [paper2](https://www.sciencedirect.com/science/article/pii/S2665963820300294?via%3Dihub))
- [SVF] https://github.com/SVF-tools/SVF
- https://github.com/harp-lab/yapall
- https://github.com/GaloisInc/cclyzerpp
- [SUTURE](https://github.com/seclab-ucr/SUTURE) - Static analysis for security
- [Phasar](https://github.com/secure-software-engineering/phasar):  a LLVM-based static analysis framework
- [yapall]https://github.com/GaloisInc/yapall
- [EOS](https://github.com/gpoesia/eos)
- https://github.com/jumormt/PSTA-16 
- [LLVM Opt Benchmark](https://github.com/dtcxzyw/llvm-opt-benchmark) - LLVM optimization benchmarks
