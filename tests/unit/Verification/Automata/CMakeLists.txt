add_lotus_test_suite(nwa_tests
  SOURCES NwaTest.cpp NwaAllocationTest.cpp
  LINK_LIBS VerificationAutomata
  TIMEOUT 120)

