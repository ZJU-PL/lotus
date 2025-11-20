cmake_minimum_required(VERSION 3.10)
project(lotus C CXX)

# Set C++14 standard and compiler flags
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti")
# -fno-rtti #we need RTTI.

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Find and configure LLVM (factored out for clarity)
include(cmake/ConfigureLLVM.cmake)

# Include dependency configuration modules
include(cmake/FindZ3.cmake)
include(cmake/ConfigureBoost.cmake)
include(cmake/ConfigureClamCrab.cmake)
include(cmake/ConfigureSVF.cmake)
include(cmake/ConfigureSeaHorn.cmake)

# Add subdirectories
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(benchmarks)

option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Add tests if enabled
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Add Python scripts and utilities
add_subdirectory(scripts)