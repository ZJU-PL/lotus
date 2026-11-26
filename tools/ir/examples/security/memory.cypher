# Memory lifetime and ownership. Run native semantic checks:
# lotus-ir-pdg-query input.bc --analysis rules --cwe 401,415,416 --format json
# --rule cpp/memory-never-freed --rule cpp/memory-may-not-be-freed
# --rule cpp/memory-leak-on-failed-call-to-realloc
# --rule cpp/return-stack-allocated-memory
# A release inventory or missing dependence path does not establish a leak.
# Generic --analysis resource-flow is an exploration heuristic; native rules
# distinguish never/may leaks, realloc alternatives, escape and object state.

MATCH (c:INST_FUNCALL) WHERE c.allocation_kind IS NOT NULL RETURN c.callee AS api, c.allocation_kind AS family, c.func AS function, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.release_kind IS NOT NULL RETURN c.callee AS api, c.release_kind AS family, c.func AS function, c.src AS location, c

# Inspect unresolved dereferences; unknown facts are not safety results.
MATCH (n:INST) WHERE n.opcode IN ['load','store'] AND n.pointer_nullness = 'unknown' RETURN n.func AS function, n.src AS location, n

# mmap has different region/unmapping semantics from C heap allocation.
# Its resource-flow inventory is separate from native heap lifetime rules.
MATCH (c:INST_FUNCALL) WHERE c.callee IN ['mmap','mmap64','munmap','mremap'] RETURN c.callee AS api, c.src AS location, c
