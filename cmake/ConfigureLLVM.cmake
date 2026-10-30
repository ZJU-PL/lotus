# Find and configure LLVM
# Preferred workflow:
#   1. Try to find a system-installed LLVM (14.x), e.g. via:
#        - system package managers (apt, Homebrew, etc.)
#        - CMake variable LLVM_DIR
#        - llvm-config on PATH
#   2. If that fails, require the user to specify LLVM_BUILD_PATH pointing to
#      the directory that contains LLVMConfig.cmake (for a local or custom build).
#
# This keeps common setups simple (no extra flags) and still supports custom LLVM builds.

# First, try to find a system LLVM without requiring an explicit path.
find_package(LLVM QUIET CONFIG)

# Validate system LLVM version (require 14.x)
if(LLVM_FOUND)
  if(NOT (LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "14.0.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "15.0.0"))
    # Version not satisfied; clear result to trigger fallback/diagnostic
    unset(LLVM_FOUND CACHE)
  endif()
endif()

# If not found or version not satisfied, try an explicit path if provided;
# otherwise, instruct the user to install or set LLVM_BUILD_PATH.
if(NOT LLVM_FOUND)
  if(LLVM_BUILD_PATH)
    # Search only under the provided path
    find_package(LLVM REQUIRED CONFIG PATHS "${LLVM_BUILD_PATH}" NO_DEFAULT_PATH)
    if(NOT (LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "14.0.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "15.0.0"))
      message(FATAL_ERROR
        "Found LLVM at LLVM_BUILD_PATH but version is ${LLVM_PACKAGE_VERSION}. "
        "This project requires LLVM 14.x")
    endif()
  else()
    message(FATAL_ERROR
      "LLVM 14.x is required but not found on the system path. "
      "Please install LLVM 14.x, or re-run CMake with "
      "-DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm")
  endif()
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

function(lotus_prune_stale_llvm_sdk_includes)
  if(NOT APPLE)
    return()
  endif()

  get_property(_lotus_imported_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
  foreach(_lotus_target IN LISTS _lotus_imported_targets)
    if(NOT _lotus_target MATCHES "^LLVM")
      continue()
    endif()

    get_target_property(_lotus_include_dirs "${_lotus_target}"
      INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _lotus_include_dirs OR
       _lotus_include_dirs STREQUAL "_lotus_include_dirs-NOTFOUND")
      continue()
    endif()

    set(_lotus_kept_include_dirs "")
    set(_lotus_removed_include_dirs "")
    foreach(_lotus_include_dir IN LISTS _lotus_include_dirs)
      if(_lotus_include_dir MATCHES "^\\$<" OR EXISTS "${_lotus_include_dir}")
        list(APPEND _lotus_kept_include_dirs "${_lotus_include_dir}")
      elseif(_lotus_include_dir MATCHES "/MacOSX\\.sdk/usr/include$")
        list(APPEND _lotus_removed_include_dirs "${_lotus_include_dir}")
      else()
        list(APPEND _lotus_kept_include_dirs "${_lotus_include_dir}")
      endif()
    endforeach()

    if(_lotus_removed_include_dirs)
      list(REMOVE_DUPLICATES _lotus_removed_include_dirs)
      message(STATUS
        "Pruning stale LLVM SDK include dirs from ${_lotus_target}: "
        "${_lotus_removed_include_dirs}")
      set_target_properties("${_lotus_target}" PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_lotus_kept_include_dirs}")
    endif()
  endforeach()
endfunction()

# Homebrew's LLVM 14 CMake exports can retain an Xcode SDK usr/include path
# that no longer exists on newer GitHub macOS runners. CMake validates imported
# target include directories during generation, so remove only that stale SDK
# path before Lotus targets link against LLVM imported targets.
lotus_prune_stale_llvm_sdk_includes()

# Derive a stable LLVM tools directory for tests and helper commands. Prefer
# the path exported by LLVMConfig.cmake; fall back to common layouts relative
# to llvm-config or LLVM_DIR when needed.
set(LOTUS_LLVM_TOOLS_BINARY_DIR "")
if(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/opt")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/clang++")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/clang++-14")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/opt-14")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND LLVM_TOOLS_BINARY_DIR)
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_DIR AND LLVM_DIR)
  get_filename_component(_lotus_llvm_prefix "${LLVM_DIR}/../../.." ABSOLUTE)
  if(EXISTS "${_lotus_llvm_prefix}/bin")
    set(LOTUS_LLVM_TOOLS_BINARY_DIR "${_lotus_llvm_prefix}/bin")
  endif()
endif()

if(LOTUS_LLVM_TOOLS_BINARY_DIR)
  message(STATUS "Using LLVM tools from ${LOTUS_LLVM_TOOLS_BINARY_DIR}")
endif()

# Include LLVM CMake modules to make functions # like add_llvm_library available (if you want to use it)
# include(AddLLVM)

# Set LLVM version-specific definitions (only 14.x supported)
add_definitions(-DLLVM14)

# Configure LLVM
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/Verification
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_BINARY_DIR}/include/Verification
  )
add_definitions(${LLVM_DEFINITIONS})
link_directories(${LLVM_LIBRARY_DIRS})
