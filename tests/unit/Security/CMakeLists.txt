lotus_add_test_group(security constanttime ConstantTime security_tests
  LINK_LIBS ConstantTimeVerify LLVMPasses LLVMTransformUtils)
lotus_add_test_group(security spectre Spectre security_tests
  LINK_LIBS Spectre LLVMPasses LLVMTransformUtils)

get_property(security_test_libs GLOBAL PROPERTY LOTUS_TEST_SUITE_security_tests_LINK_LIBS)
if(security_test_libs)
  list(REMOVE_DUPLICATES security_test_libs)
endif()
add_lotus_collected_test_suite(security_tests LINK_LIBS ${security_test_libs})