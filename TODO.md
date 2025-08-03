# Development

For Summer Research, Final Year Project Topics, etc.

## 1. Testing Improvements

- Different OS, LLVM, Z3, Boost, etc.
- Use lib/Alias/Dynamic to test pointer analyses.

## 2. Alias Analysis

### Infrastructure

- Specificatin: use lib/Annotation to enable the loading of aliasing specification files for the pointer analyses (And perhpas use dynamic analysis/ML to extract specifications)
- Data structure: integrate different representations of points-to sets.

### Interfaces

Universal Interface for the analyses in lib/Alias: 

- Basics: points-to, alias pair, alias set, pointed-by, - Callgraph, memory dependence, etc.


### Third-Party

* Integrate the pointer analyses in SVF and DG


## 3. Intermediate Representations (IR) 

### VFG (Value Flow Graph)

The DyckAA module has a DyckVFG, which is used by the null pointer analysis (lib/NullPointer).
Maybe we can design a VFG independent from DyckAA (e.g., it can use the results of other pointer analyses)

### PDG (Program Dependence Graph)

* Use the pointer analysis interfaces (currently, it relies on the memory dependence analysis inside LLVM)

## 4. Applications 

### Slicing

Use VFG or PDG to answer slicing queries.

* Program chopping
* Thin slicing

(Maybe refer to the implementation in DG)

### Bug

- Buffer overflow detection?
- Memory leak detection?
- Race condition analysis?


## 5. Numerical Analysis

* Integrate abstract interpretation frameworks (e.g., IKOS, CLAM/Crab)

## 6. Data Flow Analysis

* Revise the current data flow analysis module



## Invesgiate More Related Work

- https://github.com/harp-lab/yapall
- https://github.com/GaloisInc/cclyzerpp
- [DG](https://github.com/mchalupa/dg) - Dependence Graph for analysis of LLVM bitcode ([paper1](https://www.fi.muni.cz/~xchalup4/dg_atva20_preprint.pdf), [paper2](https://www.sciencedirect.com/science/article/pii/S2665963820300294?via%3Dihub))
- [AserPTA](https://github.com/PeimingLiu/AserPTA) - Andersen's points-to analysis
- [TPA](https://github.com/grievejia/tpa) - A flow-sensitive, context-sensitive pointer analysis
- [Andersen](https://github.com/grievejia/andersen) - Andersen's points-to analysis
- [SUTURE](https://github.com/seclab-ucr/SUTURE) - Static analysis for security
- [Phasar](https://github.com/secure-software-engineering/phasar):  a LLVM-based static analysis framework
- [EOS](https://github.com/gpoesia/eos)
- https://github.com/hotpeperoncino/sfs
- https://github.com/jumormt/PSTA-16 
- https://github.com/SVF-tools/SVF: Static Value-Flow Analysis Framework for Source Code
- [LLVM Opt Benchmark](https://github.com/dtcxzyw/llvm-opt-benchmark) - LLVM optimization benchmarks
