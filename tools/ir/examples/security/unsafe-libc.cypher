# Buffer and format checks use capacities, content facts and argument roles.
# lotus-ir-pdg-query input.bc --analysis rules --cwe 119,120,131,134,676 --format json
# --rule cpp/dangerous-function-overflow --rule cpp/potentially-dangerous-function
# --rule cpp/unsafe-strncat --rule cpp/bad-strncpy-size
# A library call's presence is an inventory, not a vulnerability finding.
# Nonconstant format strings need native provenance, not leaf inspection of a
# dependence slice. Argument positions differ across formatting APIs.

MATCH (c:INST_FUNCALL) WHERE c.copy_destination_arg IS NOT NULL RETURN c.callee AS api, c.copy_destination_arg AS destination, c.copy_source_arg AS source, c.copy_size_arg AS size, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.format_arg IS NOT NULL RETURN c.callee AS api, c.format_arg AS format_argument, c.format_expected AS expected, c.format_given AS given, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.callee IN ['gets','strcpy','strcat','sprintf','vsprintf'] RETURN c.callee AS api, c.src AS location, c
