# Optional Gurobi discovery for the ILP backend. Never required by default.
# Set GUROBI_HOME (or GUROBI_ROOT / GUROBI_DIR) to the Gurobi installation.

set(_GUROBI_HINTS
  ${GUROBI_ROOT}
  ${GUROBI_DIR}
  $ENV{GUROBI_HOME}
  $ENV{GUROBI_ROOT}
  $ENV{GUROBI_DIR})

find_path(GUROBI_INCLUDE_DIR
  NAMES gurobi_c++.h
  HINTS ${_GUROBI_HINTS}
  PATH_SUFFIXES include)

find_library(GUROBI_CPP_LIBRARY
  NAMES gurobi_c++ gurobi_c++mdd
  HINTS ${_GUROBI_HINTS}
  PATH_SUFFIXES lib lib64)

# Recent Gurobi distributions ship the core solver under a versioned name.
find_library(GUROBI_CORE_LIBRARY
  NAMES gurobi
  HINTS ${_GUROBI_HINTS}
  PATH_SUFFIXES lib lib64)

if(NOT GUROBI_CORE_LIBRARY AND GUROBI_INCLUDE_DIR)
  get_filename_component(_gurobi_prefix "${GUROBI_INCLUDE_DIR}" DIRECTORY)
  file(GLOB _gurobi_core_candidates
    "${_gurobi_prefix}/lib/libgurobi[0-9]*.so"
    "${_gurobi_prefix}/lib/libgurobi[0-9]*.dylib"
    "${_gurobi_prefix}/lib/gurobi[0-9]*.lib"
    "${_gurobi_prefix}/lib/gurobi[0-9]*.dll")
  list(SORT _gurobi_core_candidates)
  list(REVERSE _gurobi_core_candidates)
  if(_gurobi_core_candidates)
    list(GET _gurobi_core_candidates 0 GUROBI_CORE_LIBRARY)
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Gurobi
  REQUIRED_VARS GUROBI_INCLUDE_DIR GUROBI_CPP_LIBRARY)

if(Gurobi_FOUND AND NOT TARGET Gurobi::Gurobi)
  add_library(Gurobi::Gurobi UNKNOWN IMPORTED)
  set_target_properties(Gurobi::Gurobi PROPERTIES
    IMPORTED_LOCATION "${GUROBI_CPP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GUROBI_INCLUDE_DIR}")
  if(GUROBI_CORE_LIBRARY)
    set_property(TARGET Gurobi::Gurobi APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES "${GUROBI_CORE_LIBRARY}")
  endif()
endif()

mark_as_advanced(GUROBI_INCLUDE_DIR GUROBI_CPP_LIBRARY GUROBI_CORE_LIBRARY)
