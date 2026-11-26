# Lifetime findings use CFG object state, not a dependence path from free().
# Run: lotus-ir-pdg-query input.bc --analysis rules --rule cpp/double-free --rule cpp/use-after-free --format json
# Allocation-family checks: --rule cpp/new-free-mismatch
# --rule cpp/new-array-delete-mismatch --rule cpp/new-delete-array-mismatch
# Local unique_ptr/default_delete constructor, get/release/reset and destructor
# effects are modeled. Moves, custom deleters and opaque ownership stay partial.

# Model-aware inventory includes recognized C++ allocation/release symbols.
MATCH (c:INST_FUNCALL) WHERE c.allocation_kind IS NOT NULL OR c.release_kind IS NOT NULL RETURN c.callee AS api, c.allocation_kind AS allocation, c.release_kind AS release, c.src AS location, c

# Shared SSA pointer candidates. Neither CFG order nor a dereference's address
# operand is established by this query (a store can use p as its VALUE).
# Ordinary free returns void, so outgoing def-use from free is not a UAF test.
MATCH (p)-[:DATA_DEF_USE]->(f:INST_FUNCALL) MATCH (p)-[:DATA_DEF_USE]->(u:INST) WHERE f.release_kind IS NOT NULL AND u.opcode IN ['load','store','getelementptr'] RETURN p, f, u

# Two releases of a common SSA value are inspection candidates. Mutually
# exclusive branches, loop instances and allocation identity need native rules.
MATCH (p)-[:DATA_DEF_USE]->(a:INST_FUNCALL) MATCH (p)-[:DATA_DEF_USE]->(b:INST_FUNCALL) WHERE a.release_kind IS NOT NULL AND b.release_kind IS NOT NULL RETURN p, a, b
