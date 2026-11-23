# Native ABI/signature and inferred sentinel checks:
# lotus-ir-pdg-query input.bc --analysis rules --rule cpp/wrong-type-format-argument --rule cpp/unterminated-variadic-call --rule cpp/too-few-arguments --rule cpp/futile-params --rule cpp/mistyped-function-arguments --format json
# Sentinel conventions are inferred within the module; they are not universal
# contracts. Source types/casts erased by LLVM and custom format attributes
# remain unknown. The Cypher queries below select sites for inspection only.

MATCH (c:INST_FUNCALL) WHERE c.format_kind IS NOT NULL RETURN c.callee AS api, c.format_arg AS format_argument, c.format_expected AS expected, c.format_given AS given, c.src AS location, c

# Variadic actual argument counts can exceed callee_param_count legitimately.
MATCH (c:INST_FUNCALL) WHERE c.arg_count IS NOT NULL AND c.callee_param_count IS NOT NULL AND c.arg_count < c.callee_param_count RETURN c.callee AS api, c.arg_count AS actual, c.callee_param_count AS declared, c.src AS location, c
