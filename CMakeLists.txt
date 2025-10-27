cmake_minimum_required(VERSION 3.10)
project(canary C CXX)

# Set C++14 standard and compiler flags
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fexceptions")

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Find and configure LLVM
if (NOT LLVM_BUILD_PATH)
    message(FATAL_ERROR "Please specify the build folder of LLVM using -DLLVM_BUILD_PATH=")
endif()

find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_BUILD_PATH})

if(NOT LLVM_FOUND)
    message(FATAL_ERROR "Cannot find LLVMConfig.cmake under path: ${LLVM_BUILD_PATH}")
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

# Set LLVM version-specific definitions
if (LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "12.0.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "13.0.0")
    add_definitions(-DLLVM12)
elseif (LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "14.0.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "15.0.0")
    add_definitions(-DLLVM14)
else()
    message(FATAL_ERROR "Unsupported LLVM version: ${LLVM_PACKAGE_VERSION}. Supported versions are 12.x and 14.x")
endif()

# Configure LLVM
include_directories(${LLVM_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_definitions(${LLVM_DEFINITIONS})
link_directories(${LLVM_LIBRARY_DIRS})

# Include dependency configuration modules
include(cmake/FindZ3.cmake)
include(cmake/ConfigureBoost.cmake)
include(cmake/ConfigureClamCrab.cmake)
include(cmake/ConfigureSVF.cmake)

# Sea-dsa sanity checks option
option(ENABLE_SANITY_CHECKS "Enable sea-dsa sanity checks." OFF)
set(SANITY_CHECKS ${ENABLE_SANITY_CHECKS})
if(ENABLE_SANITY_CHECKS)
    message(STATUS "Compiling sea-dsa with sanity checks")
else()
    message(STATUS "Compiling sea-dsa without sanity checks")
endif()

# Add subdirectories
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(benchmarks)

option(BUILD_EXAMPLES "Build examples" OFF)
if(UILD_EXAMPLES) 
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
add_subdirectory(yaml-configurations)
