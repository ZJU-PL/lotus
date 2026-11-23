# Arithmetic candidates. Native checks use widths, promotions, mathematical
# ranges, CFG guards, scalar taint and must-alias MemorySSA definitions.
# lotus-ir-pdg-query input.bc --analysis rules --cwe 190,191,192,197,369,681 --format json
# --rule cpp/bad-addition-overflow-check
# --rule cpp/unsigned-difference-expression-compared-zero
# --rule cpp/multiplication-overflow-in-alloc
# --rule cpp/if-statement-addition-overflow
# --rule cpp/integer-multiplication-cast-to-long
# --rule cpp/divide-by-zero-using-return-value
# --rule cpp/arithmetic-with-extreme-values --rule cpp/uncontrolled-arithmetic
# --rule cpp/tainted-arithmetic --rule cpp/integer-overflow-tainted
# These Cypher selections inventory operands/sites; an arithmetic opcode or
# allocation-size role alone does not establish overflow or division by zero.

MATCH (n:INST) WHERE n.opcode IN ['add','sub','mul','shl','sdiv','udiv','srem','urem','trunc','sext','zext'] RETURN n.opcode AS operation, n.func AS function, n.src AS location, n

MATCH (c:INST_FUNCALL) WHERE c.taint_allocation_args IS NOT NULL RETURN c.callee AS api, c.taint_allocation_args AS size_arguments, c.src AS location, c
