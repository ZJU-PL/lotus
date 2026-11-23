# Native rules: --analysis rules --rule cpp/wrong-number-format-arguments
# --rule cpp/too-many-format-arguments --rule cpp/memory-unsafe-function-scan
# --rule cpp/incorrectly-checked-scanf
# Semantic format parsing: star widths/precisions, positions, escapes and
# scanf suppression. Malformed/unknown formats have no format_expected fact.
# These select equivalent supported subsets of the format argument rules.

MATCH (c:INST_FUNCALL) WHERE c.format_kind = 'printf' AND c.format_expected IS NOT NULL AND c.format_given < c.format_expected RETURN c.func AS function, c.callee AS api, c.format_expected AS expected, c.format_given AS given, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.format_kind = 'printf' AND c.format_expected IS NOT NULL AND c.format_given > c.format_expected RETURN c.func AS function, c.callee AS api, c.format_expected AS expected, c.format_given AS given, c.src AS location, c

# Unbounded, unsuppressed scanf string conversions; %%s and %9s do not match.
MATCH (c:INST_FUNCALL) WHERE c.format_kind = 'scanf' AND c.format_unbounded_string = 'true' RETURN c.func AS function, c.callee AS api, c.format_arg AS format_argument, c.src AS location, c

# Dynamic or unsupported formats require origin/taint analysis. This query
# selects the sites for exploration, without claiming attacker control.
MATCH (c:INST_FUNCALL) WHERE c.format_kind IS NOT NULL AND c.format_expected IS NULL RETURN c.func AS function, c.callee AS api, c.format_arg AS format_argument, c.src AS location, c
