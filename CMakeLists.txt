cmake_minimum_required(VERSION 3.18)
project(lotus C CXX)

include(cmake/ConfigureCompiler.cmake)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(LOTUS_TEST_BIN_DIR ${CMAKE_BINARY_DIR}/bin/tests CACHE PATH "Directory for test binaries")
set(LOTUS_EXAMPLE_BIN_DIR ${CMAKE_BINARY_DIR}/bin/examples CACHE PATH "Directory for example binaries")

include(cmake/ConfigureLLVM.cmake)
include(cmake/FindZ3.cmake)
include(cmake/LotusOptions.cmake)
include(cmake/ConfigureCoverage.cmake)

# Optional analysis dependencies.
if(LOTUS_ENABLE_CLAM OR LOTUS_ENABLE_SEAHORN OR LOTUS_ENABLE_SMACK OR
   LOTUS_ENABLE_CCLYZER OR LOTUS_ENABLE_PDAAAL)
    include(cmake/ConfigureBoost.cmake)
endif()

if(LOTUS_ENABLE_CLAM)
    include(cmake/ConfigureClamCrab.cmake)
endif()

if(LOTUS_ENABLE_SVF)
    include(cmake/ConfigureSVF.cmake)
endif()

if(LOTUS_ENABLE_SEAHORN)
    include(cmake/ConfigureSeaHorn.cmake)
endif()

if(LOTUS_ENABLE_CCLYZER)
    include(cmake/ConfigureCclyzerpp.cmake)
endif()

if(LOTUS_BUILD_TESTS)
    enable_testing()
endif()

# Third-party libraries (CUDD, WPDS, spdlog)
if(LOTUS_BUILD_ALL_LIBRARIES)
    add_subdirectory(third-party)
    add_subdirectory(lib)
else()
    # Keep targets discoverable; CMake brings in the dependency closure of
    # selected tools, tests, examples, or explicitly requested libraries.
    add_subdirectory(third-party EXCLUDE_FROM_ALL)
    add_subdirectory(lib EXCLUDE_FROM_ALL)
endif()

add_subdirectory(tools)
add_subdirectory(benchmarks)

if(LOTUS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Configure script launchers before tests register their workflow coverage.
add_subdirectory(scripts)

if(LOTUS_BUILD_TESTS)
    include(cmake/FindGTest.cmake)
    add_subdirectory(tests)
endif()

install(FILES
  config/thread.spec
  config/concurrency_api.spec
  config/openmp_api.spec
  config/mpi_api.spec
  config/linux_kernel_api.spec
  DESTINATION share/lotus/config)

include(cmake/LotusBuildAcceleration.cmake)
lotus_apply_build_acceleration()
lotus_print_build_summary()
