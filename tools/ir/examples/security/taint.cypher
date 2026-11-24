# Taint/content provenance uses native TaintAnalysis and modeled argument roles.
# lotus-ir-pdg-query input.bc --analysis rules --rule cpp/uncontrolled-process-operation --rule cpp/tainted-format-string --format json
# Other domains: --rule cpp/sql-injection --rule cpp/path-injection
# --rule cpp/command-line-injection --rule cpp/non-constant-format
# --rule cpp/uncontrolled-allocation-size --rule cpp/wordexp-injection
# Chop/slice paths explain dependences; they do not prove attacker-controlled
# contents, feasible execution or effective sanitization. Filtering a witness
# path for a sanitizer is not a sound substitute for domain-specific barriers.
# Native summaries track argument roles, overwrites, matched calls/returns and
# SQL-specific escaping. Unknown aliases/C++ APIs remain outside some models.

# Model-aware source output roles, including APIs writing through arguments.
# argv is a main parameter and stdin a global/stream, not callable APIs.
# Native taint seeds argv contents independently of this call-site inventory.
MATCH (s:INST_FUNCALL) WHERE s.taint_source_kind IS NOT NULL RETURN s.callee AS api, s.taint_source_outputs AS outputs, s.src AS location, s

MATCH (c:INST_FUNCALL) WHERE c.taint_process_args IS NOT NULL OR c.taint_command_args IS NOT NULL RETURN c.callee AS api, c.taint_process_args AS process_arguments, c.taint_command_args AS command_arguments, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.taint_format_args IS NOT NULL RETURN c.callee AS api, c.taint_format_args AS format_arguments, c.src AS location, c

# Bounded SSA return-value neighborhood, for exploration only. Memory-output
# sources need the native content model; a call's return is often just status.
MATCH (s:INST_FUNCALL)-[:DATA_DEF_USE*1..5]->(n:INST) WHERE s.taint_source_kind IS NOT NULL RETURN s, n
