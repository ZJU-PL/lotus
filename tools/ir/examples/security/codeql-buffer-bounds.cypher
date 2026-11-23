# Buffer capacity and API parameter-role checks, corresponding to the fixed
# bound branch of cpp/bad-strncpy-size. Run with -f (one query per line).
# Counts use the target ABI element size, including wide-character copies.
# Unknown capacities/bounds are excluded, not assumed safe or zero.
# Dynamic strlen/GVN checks remain available through --analysis rules.

MATCH (c:INST_FUNCALL) WHERE c.copy_size_bytes IS NOT NULL AND c.copy_destination_bytes IS NOT NULL AND c.copy_destination_bytes > 0 AND c.copy_size_bytes > c.copy_destination_bytes RETURN c.func AS function, c.callee AS api, c.copy_size_arg AS size_argument, c.copy_size_bytes AS requested_bytes, c.copy_destination_bytes AS capacity_bytes, c.src AS location, c

# Select destinations with unknown capacity for a backward slice; this is a
# coverage inventory rather than a vulnerability finding.
# --analysis slice-backward --criteria-query "... RETURN c" --edge-preset value-flow
MATCH (c:INST_FUNCALL) WHERE c.copy_size_arg IS NOT NULL AND c.copy_destination_bytes IS NULL RETURN c.callee AS api, c.func AS function, c.src AS location, c
