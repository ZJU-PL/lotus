set(certificate_test_sources
  DyckCertificatesTest.cpp
  PDACertificatesTest.cpp
  CFGCertificatesTest.cpp
  TwoNPDATest.cpp
  MatrixBackendTest.cpp
  Theorem21RandomTest.cpp
  CFGNormalizationTest.cpp
  PDAExhaustiveTest.cpp
  HardestTwoNPDATest.cpp
  FrontendTest.cpp
  LotusAdapterTest.cpp)
list(TRANSFORM certificate_test_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
lotus_collect_test_sources(cfl_tests ${certificate_test_sources})

# Imported checks invoke algorithms inside assert. Keep them active only for
# these sources, including when the parent cfl_tests target builds in Release.
if(MSVC)
  set(certificate_assert_option /UNDEBUG)
else()
  set(certificate_assert_option -UNDEBUG)
endif()
set_property(SOURCE ${certificate_test_sources}
  DIRECTORY "${PROJECT_SOURCE_DIR}/tests/unit/CFL"
  APPEND PROPERTY COMPILE_OPTIONS ${certificate_assert_option})
