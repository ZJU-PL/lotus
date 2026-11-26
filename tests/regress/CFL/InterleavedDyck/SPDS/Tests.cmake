if(TARGET lotus-cfl-interleaved-dyck)
  foreach(case crossing_forward crossing_backward mismatch spurious call_prefix
          both_prefix pending_rejected neutral all_pairs batch source target isolated
          missing_vertex invalid_label bad_number invalid_option resource_limit
          help timings missing_file missing_scope invalid_backwards_prefix
          vf_all vf_pre vf_query vf_source vf_target vf_batch vf_mismatch
          vf_endpoint vf_empty vf_prefix vf_prefix_missing_file vf_limit
          vf_invalid_client vf_missing
          nonreflexive_taint_all nonreflexive_taint_all_bidirect
          nonreflexive_taint_pre nonreflexive_taint_query
          nonreflexive_taint_source nonreflexive_taint_target
          nonreflexive_taint_batch nonreflexive_taint_self
          nonreflexive_taint_neutral nonreflexive_taint_mismatch
          nonreflexive_taint_empty nonreflexive_taint_prefix
          nonreflexive_taint_field_prefix nonreflexive_taint_limit
          nonreflexive_taint_stats nonreflexive_taint_cycle)
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
