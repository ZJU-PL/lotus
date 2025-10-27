# CLAM Tools

This directory contains three CLAM (C Language Abstract Machine) tools for LLVM bitcode analysis using abstract interpretation.

## Tools

### 1. clam
**Main CLAM analyzer** - Performs abstract interpretation-based static analysis on LLVM bitcode.

**Usage:**
```bash
clam [options] <input.bc>
```

**Key Features:**
- Multiple abstract domains (intervals, zones, octagons, polyhedra, etc.)
- Interprocedural and intraprocedural analysis
- Memory safety checking (null pointers, buffer overflows, use-after-free)
- Integration with Sea-DSA for heap analysis
- JSON output for invariants and verification results

**Common Options:**
- `--crab-dom=<domain>` - Select abstract domain (int, zones, oct, pk, etc.)
- `--crab-inter` - Enable interprocedural analysis
- `--crab-check=<type>` - Enable property checking (assert, null, bounds, uaf)
- `--crab-print-invariants=true` - Print computed invariants
- `-ojson=<file>` - Output results in JSON format
- `--help` - Display all available options

**Example:**
```bash
# Analyze with interval domain and check assertions
clam --crab-dom=int --crab-check=assert program.bc

# Interprocedural analysis with zones domain
clam --crab-inter --crab-dom=zones --crab-check=null program.bc

# Export invariants to JSON
clam --crab-print-invariants=true -ojson=results.json program.bc
```

### 2. clam-diff
**Differential analysis tool** - Compares JSON outputs from two CLAM analyses.

**Usage:**
```bash
clam-diff [options] <first.json> <second.json>
```

**Key Features:**
- Compare verification results between two analyses
- Semantic comparison using abstract domains
- Identify differences in invariants and check results
- Support for syntactic and semantic differencing

**Common Options:**
- `--dom=<domain>` - Domain for semantic comparison (int, zones, oct, pk)
- `--semdiff=<bool>` - Enable semantic differencing (default: true)
- `--verbose=<level>` - Verbosity level (0-3)

**Example:**
```bash
# Compare two analysis results
clam program.bc --crab-dom=int -ojson=v1.json
clam program.bc --crab-dom=zones -ojson=v2.json
clam-diff v1.json v2.json

# Semantic diff with zones domain
clam-diff --dom=zones --semdiff=true baseline.json current.json
```

**Workflow:**
```bash
# Generate JSON for comparison (checks):
clam.py program.c --crab-check=assert -ojson=program.json

# Generate JSON for comparison (invariants):
clam.py program.c --crab-print-invariants=true \
  --crab-print-invariants-kind=blocks -ojson=program.json
```

### 3. clam-pp
**LLVM bitcode preprocessor** - Prepares bitcode for CLAM analysis.

**Usage:**
```bash
clam-pp [options] <input.bc> -o <output.bc>
```

**Key Features:**
- Lower unsupported LLVM constructs
- Devirtualization of function calls
- Simplification transformations
- Externalization and inlining control
- Property instrumentation

**Common Options:**
- `--crab-lower-unsigned-icmp` - Lower unsigned comparisons
- `--crab-lower-select` - Lower select instructions
- `--crab-devirt` - Perform devirtualization
- `--crab-externalize-addr-taken-funcs` - Externalize address-taken functions
- `--crab-promote-assume` - Promote verifier.assume calls
- `--crab-add-invariants` - Add invariant checks to bitcode
- `-o <file>` - Output file

**Example:**
```bash
# Basic preprocessing
clam-pp program.bc -o program.prep.bc

# Full preprocessing with devirtualization
clam-pp --crab-lower-unsigned-icmp --crab-lower-select \
  --crab-devirt program.bc -o program.prep.bc

# Add invariant instrumentation
clam-pp --crab-add-invariants=all program.bc -o program.inst.bc
```

## Typical Workflow

### Basic Analysis
```bash
# 1. Preprocess (optional but recommended)
clam-pp --crab-lower-unsigned-icmp --crab-lower-select input.bc -o prep.bc

# 2. Analyze
clam --crab-dom=zones --crab-check=assert prep.bc

# 3. Export results
clam --crab-dom=zones --crab-check=assert -ojson=results.json prep.bc
```

### Differential Analysis
```bash
# Analyze baseline version
clam baseline.bc --crab-check=assert -ojson=baseline.json

# Analyze modified version
clam modified.bc --crab-check=assert -ojson=modified.json

# Compare results
clam-diff baseline.json modified.json
```

### Interprocedural Analysis with Memory Checking
```bash
# Preprocess
clam-pp --crab-devirt --crab-lower-select input.bc -o prep.bc

# Analyze with Sea-DSA heap abstraction
clam --crab-inter --crab-dom=zones \
  --crab-check=null --crab-check=bounds \
  --crab-track=mem prep.bc
```

## Integration with Lotus

These tools are integrated into the Lotus analysis framework and can be used alongside:
- Sea-DSA pointer analysis
- SVF pointer analysis  
- Other Lotus checkers and analyzers

## Dependencies

- LLVM 14.x
- CRAB (abstract interpretation library)
- Sea-DSA (heap analysis)
- Z3 SMT solver
- Boost (for some domains)

## See Also

- Main CLAM documentation: https://github.com/seahorn/clam
- CRAB library: https://github.com/seahorn/crab
- Python wrapper: `../../scripts/clam.py`
- Configuration files: `../../yaml-configurations/`

