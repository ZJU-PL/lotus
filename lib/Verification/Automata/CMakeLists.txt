add_library(VerificationAutomata STATIC
  NestedWordAutomaton.cpp
  NwaOperations.cpp
  NwaSearch.cpp
  LazySearch.cpp
  NwaBuchi.cpp
  NwaPruning.cpp
  NwaMinimization.cpp
  NwaSimulationGame.cpp
  IncrementalInclusion.cpp
)
target_include_directories(VerificationAutomata PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(VerificationAutomata PUBLIC cxx_std_17)
install(TARGETS VerificationAutomata DESTINATION lib)
