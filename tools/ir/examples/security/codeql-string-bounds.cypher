# String/content facts are evaluated immediately before each call, using the
# same MemorySSA, object aliases and format-output model as BoundsAnalysis.
# Byte lengths exclude a source string's NUL; formatted output sizes include
# its NUL. Unknown facts remain empty, not a proof of safety.

# Unbounded buffer-formatting output exceeds known object/subobject capacity.
# Destination/limit roles exclude printf streams, snprintf's bounded output,
# and asprintf's pointer-to-pointer slot. No callee-name/IR-text matching.
MATCH (c:INST_FUNCALL) WHERE c.format_output_is_buffer = true AND c.format_output_limit_arg IS NULL AND c.format_output_max_bytes IS NOT NULL AND c.arg0_object_bytes IS NOT NULL AND c.format_output_max_bytes > c.arg0_object_bytes RETURN c.callee AS api, c.format_output_max_bytes AS output_bytes_including_nul, c.arg0_object_bytes AS destination_bytes, c.format_output_value_flow AS content_or_range_estimate, c.src AS location, c

# Isolate the float-extremum case: the full %f range overflows while CodeQL's
# eight-character diagnostic estimate for each %f conversion would fit.
MATCH (c:INST_FUNCALL) WHERE c.format_output_is_buffer = true AND c.format_output_limit_arg IS NULL AND c.format_output_has_float = true AND c.format_output_max_bytes > c.arg0_object_bytes AND c.format_output_small_float_max_bytes <= c.arg0_object_bytes RETURN c.callee AS api, c.format_output_max_bytes AS full_output_bytes, c.format_output_small_float_max_bytes AS small_float_output_bytes, c.arg0_object_bytes AS destination_bytes, c.src AS location, c

# A formatting API's format argument itself must be a terminated string.
# These use semantic format-argument roles and work for mangled library calls.
MATCH (c:INST_FUNCALL) WHERE c.format_arg = 0 AND c.arg0_string_termination = 'unproven' RETURN c.callee AS api, c.arg0_string_termination AS format_state, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.format_arg = 1 AND c.arg1_string_termination = 'unproven' RETURN c.callee AS api, c.arg1_string_termination AS format_state, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.format_arg = 2 AND c.arg2_string_termination = 'unproven' RETURN c.callee AS api, c.arg2_string_termination AS format_state, c.src AS location, c

# Inspect learned source-content bounds at string-copy sites. A copy model
# alone does not say that its source requires termination (memcpy does not).
MATCH (c:INST_FUNCALL) WHERE c.copy_source_arg = 1 AND c.arg1_string_max_bytes IS NOT NULL RETURN c.callee AS api, c.arg1_string_termination AS source_state, c.arg1_string_min_bytes AS minimum_data_bytes, c.arg1_string_max_bytes AS maximum_data_bytes, c.src AS location, c
