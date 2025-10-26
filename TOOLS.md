# Binary Tools

This document describes the binary tools available in Lotus for program analysis, verification, and optimization.

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

### origin_aa

K-callsite sensitive and origin-sensitive pointer analysis tool (from https://github.com/bozhen-liu/AFG).

```bash
./origin_aa [options] <input bitcode file>
```

Key options:
- `-analysis-mode=<mode>`: Pointer analysis mode - 'ci' (context-insensitive), 'kcs' (k-callsite-sensitive), or 'origin' (origin-sensitive, default: 'ci')
- `-k=<N>`: Set k value for k-callsite-sensitive analysis (default: 1)
- `-taint`: Enable taint analysis
- `-handle-indirect`: Handle indirect calls (default: true)
- `-max-visits=<N>`: Maximum number of function visits (default: 10)
- `-print-cg`: Print call graph to callgraph.txt
- `-print-pts`: Print points-to map to pointsto.txt
- `-print-tainted`: Print tainted nodes to tainted.txt
- `-s`: Only output statistics
- `-v`: Verbose output
- `-o <file>`: Output file (default: stdout)

The tool provides three analysis modes:
1. **Context-insensitive (ci)**: Standard flow-insensitive pointer analysis
2. **K-callsite-sensitive (kcs)**: Tracks up to k call sites for better precision
3. **Origin-sensitive (origin)**: Tracks the origin of pointer values for enhanced precision

Examples:
```bash
# Context-insensitive analysis
./origin_aa input.bc

# K-callsite-sensitive analysis with k=2
./origin_aa -analysis-mode=kcs -k=2 input.bc

# Origin-sensitive analysis with taint analysis
./origin_aa -analysis-mode=origin -taint input.bc

# Generate all output files
./origin_aa -print-cg -print-pts -print-tainted input.bc
```

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

### Sea-DSA

Sea-DSA is a context-sensitive, field-sensitive pointer analysis based on DSA (Data Structure Analysis) for analyzing memory graphs and detecting memory-related issues.

#### sea-dsa-dg

A simple tool for generating memory graphs using Sea-DSA analysis.

```bash
./sea-dsa-dg [options] <input LLVM bitcode file>
```

Key options:
- `--sea-dsa-dot`: Generate DOT files visualizing the memory graphs

This tool provides a straightforward interface to the Sea-DSA analysis for generating memory graphs that can be visualized using Graphviz.

#### seadsa-tool

Advanced Sea-DSA analysis tool with comprehensive memory graph analysis capabilities.

```bash
./seadsa-tool [options] <input LLVM bitcode file>
```

Key options:
- `--sea-dsa-dot`: Generate DOT files visualizing memory graphs
- `--sea-dsa-callgraph-dot`: Generate DOT files of the complete call graph (currently disabled in this version)
- `--outdir <DIR>`: Specify an output directory for generated files

This tool provides advanced analysis capabilities for understanding memory usage patterns, pointer relationships, and potential memory-related issues in programs.

## Data Flow Analysis Tools

### ifds-taint

Interprocedural taint analysis tool using the IFDS (Interprocedural Finite Distributive Subset) framework.

```bash
./ifds-taint [options] <input bitcode file>
```

Key options:
- `-analysis=<N>`: Select analysis type (0=taint, 1=reaching-defs, default: 0)
- `-sources=<functions>`: Comma-separated list of custom source functions
- `-sinks=<functions>`: Comma-separated list of custom sink functions
- `-show-results`: Show detailed analysis results (default: true)
- `-max-results=<N>`: Maximum number of detailed results to show (default: 10)
- `-verbose`: Enable verbose output

The tool performs interprocedural taint analysis to detect potential security vulnerabilities where tainted data (from sources like user input) flows to dangerous sink functions (like system calls, memory operations, etc.).

Examples:
```bash
# Basic taint analysis
./ifds-taint input.bc

# Custom sources and sinks
./ifds-taint -sources="read,scanf" -sinks="system,exec" input.bc

# Reaching definitions analysis
./ifds-taint -analysis=1 input.bc
```

### canary-gvfa

Global Value Flow Analysis tool for vulnerability detection, including null pointer analysis and taint analysis.

```bash
./canary-gvfa [options] <input bitcode file>
```

Key options:
- `-vuln-type=<type>`: Vulnerability type to detect - 'nullpointer' or 'taint' (default: 'nullpointer')
- `-test-cfl-reachability`: Test CFL reachability queries for context-sensitive analysis
- `-dump-stats`: Dump analysis statistics
- `-verbose`: Print detailed vulnerability information

The tool uses Dyck alias analysis and value flow analysis to detect potential vulnerabilities by tracking data flow from sources to sinks. It can detect various memory bugs, such as NPD, UAF,...


Examples:
```bash
# Null pointer analysis
./canary-gvfa input.bc

# Context-sensitive analysis with statistics
./canary-gvfa -test-cfl-reachability -dump-stats -verbose input.bc
```

## Bug Finding Tools

### kint

A static bug finder for integer-related and taint-style bugs (originally from OSDI 12).

```bash
./kint [options] <input IR file>
```

Key options:

**Bug Checker Options:**
- `-check-all`: Enable all checkers (overrides individual settings)
- `-check-int-overflow`: Enable integer overflow checker
- `-check-div-by-zero`: Enable division by zero checker
- `-check-bad-shift`: Enable bad shift checker
- `-check-array-oob`: Enable array index out of bounds checker
- `-check-dead-branch`: Enable dead branch checker

**Performance Options:**
- `-function-timeout=<seconds>`: Maximum time to spend analyzing a single function (0 = no limit, default: 10)

**Logging Options:**
- `-log-level=<level>`: Set logging level (debug, info, warning, error, none, default: info)
- `-quiet`: Suppress most log output
- `-log-to-stderr`: Redirect logs to stderr instead of stdout
- `-log-to-file=<filename>`: Redirect logs to the specified file

The tool detects various integer-related bugs:
1. **Integer overflow**: Detects potential integer overflow in arithmetic operations
2. **Division by zero**: Detects potential division by zero errors
3. **Bad shift**: Detects invalid shift amounts
4. **Array out of bounds**: Detects potential array index out of bounds access
5. **Dead branch**: Detects impossible branches in conditional statements

Examples:
```bash
# Enable all checkers
./kint -check-all input.ll

# Enable specific checkers
./kint -check-int-overflow -check-div-by-zero input.ll

# Set function timeout and log level
./kint -function-timeout=30 -log-level=debug input.ll

# Quiet mode with output to file
./kint -quiet -log-to-file=analysis.log input.ll
```


## Constraint Solving Tools

### SMT Solver (owl)

```bash
./owl file.smt2
```