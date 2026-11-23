# Retained-IR static checks, including nonsecurity diagnostics:
# lotus-ir-pdg-query input.bc --analysis rules --rule cpp/power-of-10/use-of-jmp --rule cpp/power-of-10/use-of-recursion --rule cpp/jpl-c/recursion --rule cpp/incomplete-parity-check --rule cpp/equality-on-floats --rule cpp/lossy-pointer-cast --rule cpp/signed-overflow-check --rule cpp/alloca-in-loop --rule cpp/dead-code-function --rule cpp/return-value-ignored --format json
# Selectors below are inventories. Recursion needs call-graph cycles; stack
# allocation needs loop/range facts; dead functions require linkage/use facts.
# Optimized-away instructions, macro identity and source AST types may be lost.

MATCH (n:INST) WHERE n.opcode IN ['alloca','ptrtoint','fcmp','srem','icmp'] RETURN n.opcode AS operation, n.func AS function, n.src AS location, n

MATCH (c:INST_FUNCALL) WHERE c.callee IN ['setjmp','longjmp','sigsetjmp','siglongjmp','fgets'] RETURN c.callee AS api, c.func AS function, c.src AS location, c
