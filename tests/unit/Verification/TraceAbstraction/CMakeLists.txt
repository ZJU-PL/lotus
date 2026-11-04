add_lotus_test_suite(trace_abstraction_tests
  SOURCES TraceAbstractionTest.cpp MemoryAndRecursionTest.cpp
  LINK_LIBS VerificationTraceAbstraction
  TIMEOUT 120)
