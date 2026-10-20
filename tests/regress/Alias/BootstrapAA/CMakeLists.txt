if(TARGET lotus-alias-bootstrap)
  add_test(NAME bootstrap_aa_cli_smoke
    COMMAND $<TARGET_FILE:lotus-alias-bootstrap>
      --detailed-stats
      --threads=2
      ${CMAKE_SOURCE_DIR}/tests/regress/Alias/BootstrapAA/basic.ll)
  set_tests_properties(bootstrap_aa_cli_smoke PROPERTIES
    TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
    LABELS "lotus;integration;alias")
endif()

lotus_label_regression_tests(alias)
