cmake_minimum_required(VERSION 3.10.0)
project(popeye)

# Find LLVM (14.x or compatible) using CMake's find_package (same as Lotus)
find_package(LLVM CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Find Z3 (try cmake config first, then find module, manual fallback)
find_package(Z3 QUIET CONFIG)
if(NOT Z3_FOUND)
    find_package(Z3 QUIET MODULE)
endif()
if(NOT Z3_FOUND)
    find_library(Z3_LIBRARIES NAMES z3)
    find_path(Z3_INCLUDES NAMES z3++.h)
    if(Z3_LIBRARIES AND Z3_INCLUDES)
        set(Z3_FOUND TRUE)
        message(STATUS "Found Z3 via manual search: ${Z3_LIBRARIES}")
    else()
        message(FATAL_ERROR "Z3 not found. Please install Z3 (>= 4.8) or pass -DZ3_DIR=<path>")
    endif()
endif()
message(STATUS "Z3_LIBRARIES: ${Z3_LIBRARIES}")
message(STATUS "Z3_INCLUDES: ${Z3_INCLUDES}")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Seal's own headers must take priority over Lotus's (same subdirectory names)
include_directories(BEFORE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
include_directories(AFTER
    ${Z3_INCLUDES}
)

add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(benchmarks)
