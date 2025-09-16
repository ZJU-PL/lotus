# Binary Tools

This document describes the binary tools available in Lotus for program analysis, verification, and optimization.

## Bug Finding

### kint
A static bug finder for integer-related and taint-style bugs.

*(Documentation to be added)*

### canary-npa
Null pointer analysis tool.

### canary-gvfa
*(Documentation to be added)*

## Alias Analysis Tools

### DyckAA

Build and link the library files to your project and use `DyckAliasAnalysis` as a common LLVM ModulePass. 

Available options:

* `-print-alias-set-info`
  
  Prints the evaluation of alias sets and outputs all alias sets and their relations (DOT format).

* `-count-fp`
  
  Counts how many functions a function pointer may point to.

* `-no-function-type-check`
  
  If set, disables function type checking when resolving pointer calls. Otherwise, only FuncTy-compatible functions can be aliased with a function pointer. Two functions f1 and f2 are FuncTy-compatible if:
  
  - Both or neither are variadic functions
  - Both or neither have a non-void return value
  - They have the same number of parameters
  - Parameters have the same FuncTy store sizes
  - There is an explicit cast operation between FuncTy(f1) and FuncTy(f2) (works with `-with-function-cast-comb` option)

* `-dot-dyck-callgraph`
  
  Prints a call graph based on the alias analysis. Can be used with `-with-labels` option to add labels (call instructions) to the edges in call graphs.

### Sea-DSA

Sea-DSA is a context-sensitive, field-sensitive pointer analysis based on DSA (Data Structure Analysis) for analyzing memory graphs and detecting memory-related issues.

#### seadsa-dg

A simple tool for generating memory graphs:

```bash
seadsa-dg [options] <input LLVM bitcode file>
```

Options:
- `--sea-dsa-dot`: Generate DOT files visualizing the memory graphs

#### seadsa-tool

```bash
seadsa-tool [options] <input LLVM bitcode file>
```

Key options:
- `--sea-dsa-dot`: Generate DOT files visualizing memory graphs
- `--sea-dsa-callgraph-dot`: Generate DOT files of the complete call graph (currently disabled in this version)
- `--outdir <DIR>`: Specify an output directory for generated files

### FPA

The FPA tool provides several algorithms for function pointer analysis:

```bash
./fpa [options] <input bitcode files>
```

Key options:
- `-analysis-type=<N>`: Select analysis algorithm (1=FLTA, 2=MLTA, 3=MLTADF, 4=KELP)
- `-max-type-layer=<N>`: Set maximum type layer for MLTA analysis (default: 10)
- `-debug`: Enable debug output
- `-output-file=<path>`: Output file path for results (use "cout" for standard output)

Examples:
```bash
# Using FLTA analysis
./fpa -analysis-type=1 input.bc

# Using MLTA analysis with output to file
./fpa -analysis-type=2 -output-file=results.txt input.bc

# Using KELP analysis with debug info
./fpa -analysis-type=4 -debug input.bc
```

### DynAA

DynAA is a dynamic alias analysis checker that validates static alias analysis results against runtime behavior:

```bash
# 1. Compile the test program to LLVM IR
clang -emit-llvm -c example.cpp -o example.bc

# 2. Instrument the code for dynamic analysis
./bin/dynaa-instrument example.bc -o example.inst.bc

# 3. Compile the instrumented IR and link with the runtime library
clang example.inst.bc libRuntime.a -o example.inst

# 4. Run the instrumented program to collect runtime pointer information
LOG_DIR=<log-dir> ./example.inst

# 5. Check the collected information against a static alias analysis
./bin/dynaa-check example.bc <log-dir>/pts.log basic-aa
```

To dump the binary log files to a readable format:
```bash
./bin/dynaa-log-dump <log-dir>/pts.log
```

## Constraint Solving

### SMT Solver (owl)

```bash
./owl file.smt2
```

## Other Tools

### ifds-taint
*(Documentation to be added)*

### origin_aa
*(Documentation to be added)*

### sea-dsa-dg
*(Documentation to be added)*

### seadsa-tool
*(Documentation to be added)*
