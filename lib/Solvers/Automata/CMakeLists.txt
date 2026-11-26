add_library(SolversAutomata STATIC
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
target_include_directories(SolversAutomata PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(SolversAutomata PUBLIC cxx_std_17)
install(TARGETS SolversAutomata DESTINATION lib)
