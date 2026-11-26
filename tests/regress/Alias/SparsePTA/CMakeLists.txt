foreach(analysis fspta vfspta)
  add_test(NAME ${analysis}_cli_indirect_refinement
    COMMAND $<TARGET_FILE:lotus-alias-fspta>
      ${CMAKE_CURRENT_SOURCE_DIR}/indirect-refinement.ll
      --analysis=${analysis} --validate-annotations --dump-stats=false)
endforeach()

lotus_label_regression_tests(alias)
