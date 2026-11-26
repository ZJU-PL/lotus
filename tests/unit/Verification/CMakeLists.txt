add_subdirectory(Sifa)

lotus_collect_test_sources(verification_tests
  FailureDirectedTrimmingTest.cpp
  BooleanProgramFrontendTest.cpp
  Mosaic/MosaicCHCParserTest.cpp
  Mosaic/MosaicPreprocessorTest.cpp
  DriverTest.cpp)

# Component and Sifa verification tests are linked into one binary.
add_lotus_collected_test_suite(verification_tests
  LINK_LIBS
    FailureDirectedTrimming
    VerificationFrontend
    VerificationDriver
    MosaicVerification
    SifaAnalysis
    SymAbsAIAnalysis
    NPADataFlow
    CanaryAliasAnalysisWrapper
    CanaryParallel
    ${Z3_LIBRARIES}
  TIMEOUT 900)
