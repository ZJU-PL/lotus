# Native family checks (run with --analysis rules; these selections cover subsets):
# --rule cpp/missing-null-test
# --rule cpp/deref-null-result
# --rule cpp/uninitialized-local
# --rule cpp/not-initialised
# --rule cpp/conditionally-uninitialized-variable
# --rule cpp/missing-check-scanf
# --rule cpp/improper-check-return-value-scanf
# --rule cpp/late-check-of-function-argument
# --rule cpp/inconsistent-nullness-testing
# --rule cpp/inconsistent-null-check

# StateAnalysis facts immediately BEFORE a load/store dereferences its address.
# The pointer state describes the address operand, not the loaded pointer value.
# Initialization refers to the pointee before the access, not after a store.
# Unknown/unreachable facts are not treated as safe or as bugs.
# Native findings: lotus-ir-pdg-query input.bc --analysis rules --cwe 457,476
# Resource paths: --analysis rules --cwe 401,415,416,775 --format json
# Resource analysis keeps object identity, release state and compatible branch
# conditions; a generic dependence path between free calls is insufficient.

MATCH (n:INST) WHERE n.opcode IN ['load','store'] AND n.pointer_nullness IN ['null','nullable'] RETURN n.func AS function, n.opcode AS operation, n.pointer_nullness AS address_state, n.src AS location, n

MATCH (n:INST) WHERE n.opcode = 'load' AND n.pointee_initialization = 'uninitialized' RETURN n.func AS function, n.pointee_initialization AS state, n.src AS location, n

# A separate query preserves the definite versus conditional distinction.
MATCH (n:INST) WHERE n.opcode = 'load' AND n.pointee_initialization = 'maybe-uninitialized' RETURN n.func AS function, n.pointee_initialization AS state, n.src AS location, n
