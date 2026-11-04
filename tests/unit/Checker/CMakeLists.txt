lotus_add_test_group(checker ae AE checker_tests LINK_LIBS AE ${Z3_LIBRARIES})
lotus_add_test_group(checker concurrency Concurrency checker_tests
  LINK_LIBS ConcurrencyChecker)
lotus_add_test_group(checker framework Framework checker_tests LINK_LIBS CheckerFramework)
lotus_add_test_group(checker gsaf GSAF checker_tests
  LINK_LIBS GSAF LLVMTransformUtils)
lotus_add_test_group(checker kint KINT checker_tests
  LINK_LIBS CanaryKint CheckerFramework CanaryDebugInfo CanaryFormats
    CanaryLLVMUtils LLVMPasses ${Z3_LIBRARIES})
lotus_add_test_group(checker pulse Pulse checker_tests LINK_LIBS PulseChecker ${Z3_LIBRARIES})
lotus_add_test_group(checker saber Saber checker_tests LINK_LIBS Saber ${Z3_LIBRARIES})

if(LOTUS_ENABLE_TYPE_QUALIFIER)
  lotus_add_test_group(checker typequalifier TypeQualifier checker_tests
    LINK_LIBS TypeQualifier)
endif()

lotus_test_group_enabled(checker reports reports_selected)
if(reports_selected)
  lotus_collect_test_sources(checker_tests ReportFormattingTest.cpp)
  set_property(GLOBAL APPEND PROPERTY LOTUS_TEST_SUITE_checker_tests_LINK_LIBS CheckerFramework)
endif()

get_property(checker_test_libs GLOBAL PROPERTY LOTUS_TEST_SUITE_checker_tests_LINK_LIBS)
list(REMOVE_DUPLICATES checker_test_libs)
add_lotus_collected_test_suite(checker_tests
  LINK_LIBS ${checker_test_libs} COMPILE_DEFINITIONS USE_Z3)
