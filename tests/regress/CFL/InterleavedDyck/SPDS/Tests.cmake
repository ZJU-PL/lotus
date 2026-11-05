if(TARGET lotus-cfl-interleaved-dyck)
  foreach(case crossing_forward crossing_backward mismatch spurious call_prefix
          both_prefix pending_rejected neutral all_pairs batch source target isolated
          missing_vertex invalid_label bad_number invalid_option resource_limit
          help timings missing_file missing_scope invalid_backwards_prefix
          vf_all vf_pre vf_query vf_source vf_target vf_batch vf_mismatch
          vf_endpoint vf_empty vf_prefix vf_limit vf_invalid_client vf_missing)
    add_test(NAME interleaved_dyck_spds_cli_${case}
      COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:lotus-cfl-interleaved-dyck>
        -DFIXTURES=${CMAKE_CURRENT_LIST_DIR}
        -DCASE=${case}
        -P ${CMAKE_CURRENT_LIST_DIR}/RunCLI.cmake)
    set_tests_properties(interleaved_dyck_spds_cli_${case} PROPERTIES
      LABELS "lotus;integration;cfl;spds;cli" TIMEOUT 60)
  endforeach()
endif()
