# Process/command injection triage. pclose closes a stream; it is not a sink.
# exec functions execute programs directly; system/popen invoke shell commands.
# Native checks retain this distinction and exact sink argument roles:
# lotus-ir-pdg-query input.bc --analysis rules --rule cpp/uncontrolled-process-operation --rule cpp/command-line-injection --rule cpp/wordexp-injection --format json
# SQL/path checks are in codeql-taint.cypher. A sink call alone is not a bug.

MATCH (c:INST_FUNCALL) WHERE c.taint_process_args IS NOT NULL OR c.taint_command_args IS NOT NULL OR c.taint_wordexp_args IS NOT NULL RETURN c.func AS function, c.src AS location, c.callee AS api, c

# Count call sites (COUNT DISTINCT callee would count distinct API names).
MATCH (c:INST_FUNCALL) WHERE c.taint_process_args IS NOT NULL RETURN c.func AS function, COUNT(*) AS sink_count ORDER BY sink_count DESC

# Unresolved indirect calls are a coverage inventory, not injection findings.
# Unique immutable/local function-pointer targets are resolved by native rules.
MATCH (c:INST_FUNCALL {callee:'<indirect>'}) RETURN c.func AS function, c.src AS location, c
