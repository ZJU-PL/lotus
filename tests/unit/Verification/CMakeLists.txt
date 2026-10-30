add_subdirectory(Sifa)
add_subdirectory(Automata)

lotus_collect_test_sources(verification_tests
  FailureDirectedTrimmingTest.cpp
  PathProgramTest.cpp
  BooleanProgramFrontendTest.cpp
  Mosaic/MosaicCHCParserTest.cpp
  Mosaic/MosaicPreprocessorTest.cpp
  DriverTest.cpp)

# Component and Sifa verification tests are linked into one binary.
add_lotus_collected_test_suite(verification_tests
  LINK_LIBS
    FailureDirectedTrimming
    VerificationPathProgram
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
