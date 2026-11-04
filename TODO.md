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


## 3. Intermediate Representations (IR) 


### PDG (Program Dependence Graph)

* Use the pointer analysis interfaces (currently, it relies on the memory dependence analysis inside LLVM)


Implement other algorithms

- TSE 22: The Duality in Computing SSA Programs and Control Dependency
- SAS 22: Fast and Incremental Computation of Weak Control Closure
- TOPLAS 21: On Time-sensitive Control Dependencies

## 4. Applications 

### Bug Detection


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

## 5. Numerical Analysis

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
