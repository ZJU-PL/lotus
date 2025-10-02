# PDG Query Language Examples

This directory contains examples demonstrating the PDG (Program Dependence Graph) query language, which allows you to analyze program dependencies and security properties.

## Files

- `test-program.c` - A C program with various patterns for analysis
- `example-queries.txt` - Example queries and policies
- `README.md` - This file

## Building and Running

1. Compile the test program to LLVM bitcode:
   ```bash
   clang -emit-llvm -S -g test-program.c -o test-program.bc
   ```

2. Build the PDG query tool:
   ```bash
   cd /path/to/lotus
   mkdir build && cd build
   cmake ..
   make pdg-query
   ```

3. Run queries:
   ```bash
   # Interactive mode
   ./build/bin/pdg-query -i test-program.bc
   
   # Single query
   ./build/bin/pdg-query -q "pgm" test-program.bc
   
   # Policy check
   ./build/bin/pdg-query -p "noExplicitFlows(returnsOf(\"getInput\"), formalsOf(\"printOutput\")) is empty" test-program.bc
   
   # Batch queries from file
   ./build/bin/pdg-query -f example-queries.txt test-program.bc
   ```

## Query Language Syntax

### Basic Expressions

- `pgm` - The entire program dependence graph
- `forwardSlice(expr)` - Forward slice from expression
- `backwardSlice(expr)` - Backward slice from expression
- `shortestPath(expr1, expr2)` - Shortest path between expressions
- `selectNodes(nodeType)` - Select nodes of specific type
- `selectEdges(edgeType)` - Select edges of specific type

### Function Queries

- `returnsOf("functionName")` - Return values of a function
- `formalsOf("functionName")` - Formal parameters of a function
- `entriesOf("functionName")` - Entry points of a function

### Set Operations

- `expr1 U expr2` - Union of two expressions
- `expr1 ∩ expr2` - Intersection of two expressions
- `expr1 - expr2` - Difference of two expressions

### Variable Binding

- `let var = expr1 in expr2` - Bind variable in expression

### Policy Checks

- `expr is empty` - Check if expression evaluates to empty set
- `expr is not empty` - Check if expression is non-empty

## Example Queries

### Basic Analysis

```bash
# Get all nodes in the PDG
pgm

# Get all function call nodes
selectNodes(INST_FUNCALL)

# Get all data dependency edges
selectEdges(DATA_DEF_USE)

# Get return values of main function
returnsOf("main")
```

### Information Flow Analysis

```bash
# Check for direct flows from input to output
between(returnsOf("getInput"), formalsOf("printOutput"))

# Check for flows from secret to network
between(returnsOf("getSecret"), formalsOf("networkSend"))

# Forward slice from user input
forwardSlice(returnsOf("getInput"))

# Backward slice to output
backwardSlice(formalsOf("printOutput"))
```

### Security Policies

```bash
# No explicit flows from secret to output
noExplicitFlows(returnsOf("getSecret"), formalsOf("printOutput")) is empty

# No flows from input to network without sanitization
let sources = returnsOf("getInput") in
let sinks = formalsOf("networkSend") in
let sanitizers = returnsOf("sanitize") in
declassifies(sanitizers, sources, sinks) is empty

# Access control: sensitive operations only when authorized
let checks = findPCNodes(returnsOf("isAuthorized"), TRUE) in
let sensitiveOps = selectNodes(INST_FUNCALL) in
accessControlled(checks, sensitiveOps) is empty
```

### Complex Queries

```bash
# Find all paths from input to output through sanitization
let input = returnsOf("getInput") in
let output = formalsOf("printOutput") in
let sanitized = returnsOf("sanitize") in
let path1 = between(input, sanitized) in
let path2 = between(sanitized, output) in
path1 U path2

# Find nodes that are both data and control dependent
let dataNodes = selectNodes(INST_FUNCALL) in
let controlNodes = selectNodes(INST_BR) in
dataNodes ∩ controlNodes
```

## Node Types

- `INST_FUNCALL` - Function call instructions
- `INST_RET` - Return instructions
- `INST_BR` - Branch instructions
- `INST_OTHER` - Other instructions
- `FUNC_ENTRY` - Function entry points
- `PARAM_FORMALIN` - Formal input parameters
- `PARAM_FORMALOUT` - Formal output parameters
- `PARAM_ACTUALIN` - Actual input parameters
- `PARAM_ACTUALOUT` - Actual output parameters
- `FUNC` - Function nodes
- `CLASS` - Class nodes

## Edge Types

- `DATA_DEF_USE` - Data definition-use edges
- `DATA_RAW` - Read-after-write edges
- `DATA_READ` - Read edges
- `DATA_ALIAS` - Alias edges
- `CONTROLDEP_BR` - Control dependency from branches
- `CONTROLDEP_ENTRY` - Control dependency from entry
- `PARAMETER_IN` - Parameter input edges
- `PARAMETER_OUT` - Parameter output edges
- `GLOBAL_DEP` - Global dependency edges

## Security Analysis Examples

The test program contains several security-relevant patterns:

1. **Direct Information Flow**: Input flows directly to output
2. **Controlled Access**: Secret only accessed when authorized
3. **Sanitized Flow**: Input sanitized before network transmission
4. **Uncontrolled Secret Flow**: Secret leaked based on input
5. **Sensitive Data Logging**: Secret always logged
6. **Complex Control Flow**: Authorization checks with data processing

Use the query language to detect these patterns and verify security properties.
