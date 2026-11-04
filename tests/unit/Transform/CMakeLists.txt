lotus_add_test_group(transform nisse Nisse transform_tests
  LINK_LIBS CanaryNisse LLVMPasses LLVMTransformUtils)

get_property(transform_test_libs GLOBAL PROPERTY LOTUS_TEST_SUITE_transform_tests_LINK_LIBS)
if(transform_test_libs)
  list(REMOVE_DUPLICATES transform_test_libs)
endif()
add_lotus_collected_test_suite(transform_tests LINK_LIBS ${transform_test_libs})