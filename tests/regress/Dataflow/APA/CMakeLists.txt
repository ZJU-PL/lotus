if(TARGET lotus-dfa-apa)
  set(APA_ORDER_CLI_INPUT
    "${PROJECT_SOURCE_DIR}/tests/regress/Dataflow/APA/order-policies.ll")
  foreach(scope reachable inter_reachable inter_affine)
    foreach(policy structural expr-aware star-risk hybrid rpo random min-degree)
      set(test_name "apa_cli.${scope}.${policy}")
      add_test(NAME "${test_name}" COMMAND $<TARGET_FILE:lotus-dfa-apa>
        "${APA_ORDER_CLI_INPUT}" "--analysis=${scope}" "--ordering=${policy}"
        --stdout --measure-peak --order-trace)
      set_tests_properties("${test_name}" PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[order-step\\]"
        LABELS "lotus;integration;dataflow;apa-cli" TIMEOUT 60)
    endforeach()
  endforeach()
  foreach(policy structural expr-aware star-risk hybrid)
    set(test_name "apa_cli.modular.${policy}")
    add_test(NAME "${test_name}" COMMAND $<TARGET_FILE:lotus-dfa-apa>
      "${APA_ORDER_CLI_INPUT}" --analysis=inter_reachable "--ordering=${policy}"
      --inter-engine=modular --stdout --measure-peak --order-trace)
    set_tests_properties("${test_name}" PROPERTIES
      PASS_REGULAR_EXPRESSION "\\[order-step\\]"
      LABELS "lotus;integration;dataflow;apa-cli" TIMEOUT 60)
  endforeach()
  foreach(error_case unknown_policy invalid_cap invalid_permutation)
    if(error_case STREQUAL "unknown_policy")
      set(error_args --ordering=unknown)
    elseif(error_case STREQUAL "invalid_cap")
      set(error_args --ordering=hybrid --order-star-cap=0)
    else()
      set(error_args --ordering=explicit --order-explicit=0,0)
    endif()
    add_test(NAME "apa_cli.${error_case}" COMMAND $<TARGET_FILE:lotus-dfa-apa>
      "${APA_ORDER_CLI_INPUT}" ${error_args} --stdout)
    set_tests_properties("apa_cli.${error_case}" PROPERTIES WILL_FAIL TRUE
      LABELS "lotus;integration;dataflow;apa-cli" TIMEOUT 60)
  endforeach()
endif()

lotus_label_regression_tests(dataflow)
