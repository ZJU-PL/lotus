# Find and configure Z3
find_library(Z3_LIBRARIES NAMES z3 HINTS ${Z3_DIR} ENV Z3_DIR PATH_SUFFIXES bin lib)
find_path(Z3_INCLUDES NAMES z3++.h HINTS ${Z3_DIR} ENV Z3_DIR PATH_SUFFIXES include z3)

if(NOT Z3_LIBRARIES OR NOT Z3_INCLUDES)
    message(FATAL_ERROR "Z3 not found!")
endif()

message(STATUS "Found Z3: ${Z3_LIBRARIES}")
include_directories(${Z3_INCLUDES})

