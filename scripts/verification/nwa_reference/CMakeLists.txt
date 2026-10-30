# External reference validation is separate from the native unit-test suites.
# Build explicitly; running it also requires a user-supplied Ultimate checkout.
add_executable(nwa_differential_runner EXCLUDE_FROM_ALL NwaOracle.cpp)
target_link_libraries(nwa_differential_runner PRIVATE VerificationAutomata)
set_target_properties(nwa_differential_runner PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
