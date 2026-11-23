# Queries that can guide a coding agent toward relevant code, dataflow, and context.
#
# Before writing Cypher queries, discover the schema:
#   lotus-ir-pdg-query --schema
# This prints all node labels, edge types, properties, group labels, and edge
# presets as JSON.  LLM agents should call --schema at the start of a session
# to avoid guessing label/property names.
#
# Quick reference (also see tools/ir/examples/README.md for a mini schema):
#   Node labels: :INST_FUNCALL :INST_RET :INST_BR :INST_OTHER :FUNC_ENTRY
#                :PARAM_FORMALIN :PARAM_FORMALOUT :PARAM_ACTUALIN :PARAM_ACTUALOUT
#                :VAR_* :FUNC :CLASS :ANNO_*
#   Node groups: :INST :VAR :PARAM :ANNO
#   Edge types:  :DATA_DEF_USE :DATA_RAW :DATA_READ :DATA_ALIAS :DATA_RET
#                :CONTROLDEP_* :IND_CALL :PARAMETER_IN :PARAMETER_OUT :PARAMETER_FIELD
#                :GLOBAL_DEP :VAL_DEP :CLS_MTH :ANNO_*
#   Edge groups: :CONTROL_DEP :CALL :DATA_DEP
#
# Security triage examples (coverage and limitations: COVERAGE.md):
#   security/injection.cypher     — command injection detection
#   security/memory.cypher        — UAF, double-free, memory leaks
#   security/unsafe-libc.cypher   — strcpy/gets/sprintf + format string
#   security/resource.cypher      — resource leaks, API pairing
#   security/double-free.cypher   — double-free/UAF (focused, with chop patterns)
#   security/taint.cypher         — input-to-sink taint tracking
# See each file for prerequisites and CLI invocation examples.

# "Where are the sensitive calls?" (example: system)
MATCH (s:INST_FUNCALL {callee:"system"}) RETURN s.func AS func, s.src AS src, s.llvm AS ir LIMIT 50

# Immediate def-use neighbors of a sensitive call (1-hop data-dependence out-edges)
MATCH (s:INST_FUNCALL {callee:"system"})-[:DATA_DEP]->(n) RETURN n.label AS kind, n.func AS func, n.src AS src, n.llvm AS ir LIMIT 200

# Immediate control-dependence neighborhood of a branch (often highlights guards)
MATCH (b:INST_BR)-[:CONTROL_DEP]->(n) RETURN n.label AS kind, n.func AS func, n.src AS src, n.llvm AS ir LIMIT 200

# Incoming SSA argument dependencies; parameter-tree layout depends on debug/type information.
MATCH (p)-[:DATA_DEF_USE]->(c:INST_FUNCALL) RETURN p.label AS kind, c.func AS func LIMIT 200
