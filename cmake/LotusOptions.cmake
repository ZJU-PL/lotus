# Core build surfaces
option(LOTUS_BUILD_EXAMPLES "Build examples" OFF)
option(LOTUS_BUILD_TESTS "Build tests" OFF)
option(LOTUS_BUILD_ALL_LIBRARIES
       "Include all libraries in the default build, even if no selected consumer needs them" ON)
set(LOTUS_TOOL_FAMILIES "all" CACHE STRING
    "Semicolon-separated tool families: all, alias, checker, dataflow, optimization, solver, verifier, ir, cfl; empty builds no tools")
set(LOTUS_TEST_SUBSYSTEMS "all" CACHE STRING
    "Semicolon-separated test subsystems or groups, e.g. concurrency/cuda;dataflow/vasco;analysis/cfg; all selects everything")
option(LOTUS_ENABLE_TIME_TRACE "Emit Clang compilation time traces for Lotus C++ targets" OFF)
set(LOTUS_PCH_TARGETS "" CACHE STRING
    "Semicolon-separated Lotus targets using private LLVM/STL precompiled headers")
set(LOTUS_UNITY_TARGETS "" CACHE STRING
    "Semicolon-separated Lotus targets using experimental unity builds")
set(LOTUS_UNITY_BATCH_SIZE 4 CACHE STRING "Maximum sources per selected unity batch")

# SparrowAA, DyckAA, UnderApprox and LLVM's CFL analyses remain available.
# These switches affect the wrapper only; standalone backend libraries/tools
# are still available and can be built explicitly.
foreach(backend DDA TPA CCLYZER)
  option(LOTUS_AA_WRAPPER_ENABLE_${backend}
         "Include ${backend} in the unified alias-analysis wrapper" ON)
endforeach()
option(LOTUS_ENABLE_COVERAGE
       "Instrument Lotus and its tests for LLVM source coverage" OFF)
set(LOTUS_COVERAGE_MINIMUM 0 CACHE STRING
    "Minimum total line coverage percentage required")

# Optional analysis and verification integrations.
option(LOTUS_ENABLE_IKOS_API_SPEC "Install the IKOS API-spec stub generation tools" OFF)
option(LOTUS_ENABLE_IKOS "Build the IKOS analyzer and API-spec pipeline" OFF)
option(LOTUS_IKOS_ENABLE_APRON "Enable optional APRON domains in IKOS" OFF)
if(LOTUS_ENABLE_IKOS)
  set(LOTUS_ENABLE_IKOS_API_SPEC ON)
endif()
option(LOTUS_ENABLE_CLAM "Enable CLAM abstract interpretation framework" OFF)
option(LOTUS_ENABLE_SEAHORN "Enable SeaHorn" OFF)
option(LOTUS_ENABLE_SMACK
       "Enable SMACK LLVM-to-Boogie verifier frontend" OFF)
option(LOTUS_ENABLE_SVF "Enable SVF" OFF)
option(LOTUS_ENABLE_CCLYZER
       "Enable optional cclyzer++ alias analysis backend" OFF)
if(DEFINED LOTUS_USE_CCLYZER)
  set(LOTUS_ENABLE_CCLYZER ${LOTUS_USE_CCLYZER} CACHE BOOL
      "Enable optional cclyzer++ alias analysis backend" FORCE)
endif()

# Optional in-tree components
option(LOTUS_ENABLE_TYPE_QUALIFIER
       "Enable the opt-in TypeQualifier uninitialized-data checker" OFF)
option(LOTUS_ENABLE_FPSOLVE
       "Build the vendored FPsolve library under third-party/fpsolve" OFF)
option(LOTUS_ENABLE_WALI_OPENNWA
       "Build the vendored WALi/OpenNWA library under third-party/WALi-OpenNWA"
       OFF)
option(LOTUS_ENABLE_CFL "Build CFL reachability solvers" ON)
option(LOTUS_ENABLE_CSR
       "Build the indexing context-sensitive reachability solver" OFF)
option(LOTUS_ENABLE_OWL "Build Owl SMT solver" OFF)
option(LOTUS_ENABLE_SMT_STABILIZER
       "Build the SMTStabilizer SMT-LIB normalization library and tool" OFF)
option(LOTUS_ENABLE_DYNAA "Build dynamic alias analyses" OFF)
option(LOTUS_ENABLE_HORN_ICE
       "Build ICE learning for CHC and Boogie" OFF)
option(LOTUS_ENABLE_SEAL
       "Build the Seal symbolic automata lifter under third-party/seal" OFF)
option(LOTUS_ENABLE_PDAAAL
       "Build the vendored PDAAAL weighted PDS reachability library under third-party/PDAAAL" OFF)
option(LOTUS_ENABLE_FLOW_CUTTER
       "Build the vendored PACE 2017 treewidth and PACE 2020 treedepth tools" OFF)
option(LOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS
       "Build the Boolean-program grammar parser and normalizer" OFF)
option(LOTUS_ENABLE_DEMAND_APA
       "Build the independent OOPSLA artifact demand-driven APA executable" OFF)

# Advanced toggles
option(LOTUS_DOWNLOAD_BOOST "Download and build Boost if not found" ON)
option(LOTUS_DOWNLOAD_CRAB "Download and build CRAB if not found" OFF)
option(LOTUS_SEAHORN_BUILD_32_BIT_RT "Build 32-bit SeaHorn runtime libraries"
       OFF)
option(LOTUS_SEADSA_ENABLE_SANITY_CHECKS
       "Enable Sea-dsa sanity checks" OFF)
option(LOTUS_WPDS_WITNESS_TRACE
       "Enable WPDS witness tracing in WPDSDataFlow" OFF)
option(LOTUS_EGRAPH_ENABLE_DOT
       "Enable DOT/Graphviz helpers in Lotus EGraph" ON)
option(LOTUS_EGRAPH_ENABLE_JSON
       "Enable JSON serialization helpers in Lotus EGraph" ON)

# User-provided dependency overrides
set(LOTUS_CUSTOM_BOOST_ROOT "" CACHE PATH
    "Path to a custom Boost installation")
set(LOTUS_CUSTOM_CRAB_ROOT "" CACHE PATH
    "Path to a custom CRAB installation")

# Validate selections here with the options they control.
function(lotus_resolve_selection option_name available out_var)
  string(TOLOWER "${${option_name}}" selected)
  if(selected STREQUAL "all")
    if(ARGN)
      set(selected ${ARGN})
    else()
      set(selected ${available})
    endif()
  else()
    foreach(component IN LISTS selected)
      if(NOT component IN_LIST available)
        message(FATAL_ERROR
          "Unknown component '${component}' in ${option_name}. "
          "Choose from: all;${available}")
      endif()
    endforeach()
    list(REMOVE_DUPLICATES selected)
  endif()
  set(${out_var} "${selected}" PARENT_SCOPE)
endfunction()

set(_lotus_test_roots
  alias analysis checker cfl concurrency dataflow fuzzing ir
  security solvers symbolicexecution transform utils verification)
set(_lotus_test_choices ${_lotus_test_roots})
foreach(group dda aserpta bootstrapaa flowsensitive lotusaa sparrowaa tpa
              cclyzeraa dyckaa seadsa allocaa underapproxaa ptsset
              aliaswrapper)
  list(APPEND _lotus_test_choices "alias/${group}")
endforeach()
foreach(group cfg controldependence debuginfo multiplicity nullpointer
              parametersummary profile purity sccp typehierarchy loop)
  list(APPEND _lotus_test_choices "analysis/${group}")
endforeach()
foreach(group mhp lockset valueflow thread clocks threadapi threadlocal openmp
              mpi cuda linuxkernel)
  list(APPEND _lotus_test_choices "concurrency/${group}")
endforeach()
foreach(group ae concurrency framework gsaf kint pulse saber reports typequalifier)
  list(APPEND _lotus_test_choices "checker/${group}")
endforeach()
foreach(group ifdside mono wpds apa demandapa npa vasco)
  list(APPEND _lotus_test_choices "dataflow/${group}")
endforeach()

foreach(group constanttime spectre)
  list(APPEND _lotus_test_choices "security/${group}")
endforeach()
foreach(group nisse)
  list(APPEND _lotus_test_choices "transform/${group}")
endforeach()

lotus_resolve_selection(LOTUS_TOOL_FAMILIES
  "alias;checker;dataflow;optimization;solver;verifier;ir;cfl"
  LOTUS_SELECTED_TOOL_FAMILIES)
lotus_resolve_selection(LOTUS_TEST_SUBSYSTEMS "${_lotus_test_choices}"
  LOTUS_SELECTED_TEST_SUBSYSTEMS ${_lotus_test_roots})

if(NOT LOTUS_ENABLE_CFL)
  string(TOLOWER "${LOTUS_TOOL_FAMILIES}" requested_tools)
  string(TOLOWER "${LOTUS_TEST_SUBSYSTEMS}" requested_tests)
  if("cfl" IN_LIST LOTUS_SELECTED_TOOL_FAMILIES AND
     NOT requested_tools STREQUAL "all")
    message(FATAL_ERROR "Selecting cfl tools requires LOTUS_ENABLE_CFL=ON")
  endif()
  if(LOTUS_BUILD_TESTS AND "cfl" IN_LIST LOTUS_SELECTED_TEST_SUBSYSTEMS AND
     NOT requested_tests STREQUAL "all")
    message(FATAL_ERROR "Selecting cfl tests requires LOTUS_ENABLE_CFL=ON")
  endif()
  list(REMOVE_ITEM LOTUS_SELECTED_TOOL_FAMILIES cfl)
  list(REMOVE_ITEM LOTUS_SELECTED_TEST_SUBSYSTEMS cfl)
endif()
if(LOTUS_BUILD_TESTS AND "checker/typequalifier" IN_LIST LOTUS_SELECTED_TEST_SUBSYSTEMS
   AND NOT LOTUS_ENABLE_TYPE_QUALIFIER)
  message(FATAL_ERROR "Selecting checker/typequalifier requires LOTUS_ENABLE_TYPE_QUALIFIER=ON")
endif()

set(LOTUS_SELECTED_TEST_ROOTS)
foreach(selection IN LISTS LOTUS_SELECTED_TEST_SUBSYSTEMS)
  string(REGEX REPLACE "/.*$" "" subsystem "${selection}")
  list(APPEND LOTUS_SELECTED_TEST_ROOTS "${subsystem}")
endforeach()
list(REMOVE_DUPLICATES LOTUS_SELECTED_TEST_ROOTS)

function(lotus_test_group_enabled subsystem group out_var)
  if(subsystem IN_LIST LOTUS_SELECTED_TEST_SUBSYSTEMS OR
     "${subsystem}/${group}" IN_LIST LOTUS_SELECTED_TEST_SUBSYSTEMS)
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(_lotus_summary_bool label value)
  if(${value})
    message(STATUS "  ${label}: ON")
  else()
    message(STATUS "  ${label}: OFF")
  endif()
endfunction()

function(lotus_print_build_summary)
  message(STATUS "")
  message(STATUS "Lotus build summary")
  message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
  message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
  message(STATUS "  Binary dir: ${CMAKE_BINARY_DIR}")
  message(STATUS "  LLVM package: ${LLVM_PACKAGE_VERSION}")
  _lotus_summary_bool("Build tests" LOTUS_BUILD_TESTS)
  _lotus_summary_bool("Coverage instrumentation" LOTUS_ENABLE_COVERAGE)
  _lotus_summary_bool("Build examples" LOTUS_BUILD_EXAMPLES)
  _lotus_summary_bool("Build all libraries" LOTUS_BUILD_ALL_LIBRARIES)
  message(STATUS "  Tool families: ${LOTUS_TOOL_FAMILIES}")
  message(STATUS "  Test subsystems: ${LOTUS_TEST_SUBSYSTEMS}")
  _lotus_summary_bool("Compilation time traces" LOTUS_ENABLE_TIME_TRACE)
  message(STATUS "  PCH targets: ${LOTUS_PCH_TARGETS}")
  message(STATUS "  Unity targets: ${LOTUS_UNITY_TARGETS}")
  foreach(backend DDA TPA CCLYZER)
    _lotus_summary_bool("Alias wrapper ${backend}" LOTUS_AA_WRAPPER_ENABLE_${backend})
  endforeach()
  message(STATUS "  Optional tool families:")
  _lotus_summary_bool("CFL libraries and tools" LOTUS_ENABLE_CFL)
  _lotus_summary_bool("CSR tool" LOTUS_ENABLE_CSR)
  _lotus_summary_bool("Owl SMT tool" LOTUS_ENABLE_OWL)
  _lotus_summary_bool("SMTStabilizer" LOTUS_ENABLE_SMT_STABILIZER)
  _lotus_summary_bool("DynAA tools" LOTUS_ENABLE_DYNAA)
  message(STATUS "  Optional integrations:")
  _lotus_summary_bool("IKOS API-spec tools" LOTUS_ENABLE_IKOS_API_SPEC)
  _lotus_summary_bool("IKOS analyzer" LOTUS_ENABLE_IKOS)
  _lotus_summary_bool("CLAM" LOTUS_ENABLE_CLAM)
  _lotus_summary_bool("SeaHorn" LOTUS_ENABLE_SEAHORN)
  _lotus_summary_bool("SMACK" LOTUS_ENABLE_SMACK)
  _lotus_summary_bool("Horn-ICE" LOTUS_ENABLE_HORN_ICE)
  _lotus_summary_bool("Seal/Popeye" LOTUS_ENABLE_SEAL)
  _lotus_summary_bool("PDAAAL" LOTUS_ENABLE_PDAAAL)
  _lotus_summary_bool("FlowCutter" LOTUS_ENABLE_FLOW_CUTTER)
  _lotus_summary_bool("Boolean program tools" LOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS)
  _lotus_summary_bool("DemandAPA" LOTUS_ENABLE_DEMAND_APA)
  _lotus_summary_bool("SVF" LOTUS_ENABLE_SVF)
  _lotus_summary_bool("Cclyzer++" LOTUS_ENABLE_CCLYZER)
  message(STATUS "  Advanced toggles:")
  _lotus_summary_bool("TypeQualifier" LOTUS_ENABLE_TYPE_QUALIFIER)
  _lotus_summary_bool("FPsolve" LOTUS_ENABLE_FPSOLVE)
  _lotus_summary_bool("WALi/OpenNWA" LOTUS_ENABLE_WALI_OPENNWA)
  _lotus_summary_bool("Download Boost" LOTUS_DOWNLOAD_BOOST)
  _lotus_summary_bool("Download CRAB" LOTUS_DOWNLOAD_CRAB)
  _lotus_summary_bool("SeaDsa sanity checks" LOTUS_SEADSA_ENABLE_SANITY_CHECKS)
  _lotus_summary_bool("WPDS witness trace" LOTUS_WPDS_WITNESS_TRACE)
  _lotus_summary_bool("SeaHorn 32-bit runtime" LOTUS_SEAHORN_BUILD_32_BIT_RT)
  _lotus_summary_bool("EGraph DOT" LOTUS_EGRAPH_ENABLE_DOT)
  _lotus_summary_bool("EGraph JSON" LOTUS_EGRAPH_ENABLE_JSON)
endfunction()
