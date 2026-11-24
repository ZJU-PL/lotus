# Native family checks (run with --analysis rules; these selections cover subsets):
# --rule cpp/overflow-buffer
# --rule cpp/static-buffer-overflow
# --rule cpp/invalid-pointer-deref
# --rule cpp/no-space-for-terminator
# --rule cpp/constant-array-overflow
# --rule cpp/improper-null-termination
# --rule cpp/user-controlled-null-termination-tainted
# --rule cpp/overflowing-snprintf
# --rule cpp/overflow-calculated
# --rule cpp/overflow-destination
# --rule cpp/unbounded-write
# --rule cpp/overrunning-write
# --rule cpp/very-likely-overrunning-write
# --rule cpp/overrunning-write-with-float
# --rule cpp/badly-bounded-write
# --rule cpp/potential-buffer-overflow
# --rule cpp/unclear-array-index-validation

# Object/subobject capacity and access widths, shared with BoundsAnalysis.
# Run: lotus-ir-pdg-query input.bc -f tools/ir/examples/security/codeql-memory-access.cypher
# Unknown sizes/offsets stay unknown. False means no proven bad access among
# known accesses; it does not prove an instruction's unknown accesses safe.
# Numeric access_* facts are available when exactly one access is known.
# Native --analysis rules --cwe 119,125,131,787 also handles symbolic allocation
# ends and missing terminator space, which these fixed-capacity predicates omit.

MATCH (n:INST) WHERE n.write_out_of_bounds = true RETURN n.func AS function, n.opcode AS operation, n.access_bytes AS bytes, n.access_capacity_bytes AS subobject_bytes, n.access_offset_bytes AS offset, n.src AS location, n

MATCH (n:INST) WHERE n.read_out_of_bounds = true RETURN n.func AS function, n.opcode AS operation, n.access_bytes AS bytes, n.access_capacity_bytes AS subobject_bytes, n.access_offset_bytes AS offset, n.src AS location, n

# Known negative offsets are retained, rather than wrapped to an unsigned size.
MATCH (n:INST) WHERE n.access_offset_bytes < 0 AND n.access_bytes > 0 RETURN n.func AS function, n.access_offset_bytes AS offset, n.access_bytes AS bytes, n.src AS location, n
