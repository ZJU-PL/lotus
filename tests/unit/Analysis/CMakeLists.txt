# Analysis Unit Tests

set(analysis_test_dependencies)
set(analysis_test_definitions)
lotus_test_group_enabled(analysis loop loop_selected)
lotus_test_group_enabled(analysis typehierarchy type_hierarchy_selected)

if(loop_selected)

set(NOELLE_LOOP_PARITY_SRC_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/noelle_parity)
set(NOELLE_LOOP_PARITY_LL_DIR
    ${CMAKE_BINARY_DIR}/tests/unit/noelle_loop_parity_ll)

file(MAKE_DIRECTORY ${NOELLE_LOOP_PARITY_LL_DIR})

if(APPLE)
  if(EXISTS /opt/homebrew/opt/llvm@14/bin/clang++)
    set(NOELLE_LOOP_PARITY_CLANGXX /opt/homebrew/opt/llvm@14/bin/clang++)
  elseif(EXISTS /usr/local/opt/llvm@14/bin/clang++)
    set(NOELLE_LOOP_PARITY_CLANGXX /usr/local/opt/llvm@14/bin/clang++)
  else()
    execute_process(
        COMMAND xcrun --sdk macosx --find clang++
        OUTPUT_VARIABLE NOELLE_LOOP_PARITY_CLANGXX
        OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  if(EXISTS /opt/homebrew/opt/llvm@14/bin/opt)
    set(NOELLE_LOOP_PARITY_OPT /opt/homebrew/opt/llvm@14/bin/opt)
  elseif(EXISTS /usr/local/opt/llvm@14/bin/opt)
    set(NOELLE_LOOP_PARITY_OPT /usr/local/opt/llvm@14/bin/opt)
  else()
    find_program(NOELLE_LOOP_PARITY_OPT opt REQUIRED)
  endif()
  execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE NOELLE_LOOP_PARITY_SYSROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(NOELLE_LOOP_PARITY_EXTRA_ARGS -isysroot ${NOELLE_LOOP_PARITY_SYSROOT})
else()
  find_program(NOELLE_LOOP_PARITY_CLANGXX
               NAMES clang++-14 clang++
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR})
  if(NOT NOELLE_LOOP_PARITY_CLANGXX)
    set(NOELLE_LOOP_PARITY_CLANGXX ${CMAKE_CXX_COMPILER})
  endif()
  find_program(NOELLE_LOOP_PARITY_OPT
               NAMES opt-14 opt
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR}
               REQUIRED)
  set(NOELLE_LOOP_PARITY_EXTRA_ARGS)
endif()

set(NOELLE_LOOP_PARITY_CASES
    iv_attributes/nested_loop_governing
    sccdag_attributes/simple
    sccdag_attributes/reducible_addition
    sccdag_attributes/clonable_alloca
    loop_domain_space/one_dim_array
    loop_domain_space/two_dim_matrix)

set(NOELLE_LOOP_PARITY_LL_OUTPUTS)
foreach(case_name ${NOELLE_LOOP_PARITY_CASES})
  set(src_file ${NOELLE_LOOP_PARITY_SRC_DIR}/${case_name}/test.cpp)
  string(REPLACE "/" "_" case_output ${case_name})
  set(pre_bc ${NOELLE_LOOP_PARITY_LL_DIR}/${case_output}_pre.bc)
  set(ll_file ${NOELLE_LOOP_PARITY_LL_DIR}/${case_output}.ll)
  list(APPEND NOELLE_LOOP_PARITY_LL_OUTPUTS ${ll_file})
  add_custom_command(
      OUTPUT ${ll_file}
      COMMAND ${NOELLE_LOOP_PARITY_CLANGXX}
              ${NOELLE_LOOP_PARITY_EXTRA_ARGS}
              -std=c++17 -emit-llvm -c -g -O0 -Xclang -disable-O0-optnone
              ${src_file} -o ${pre_bc}
      COMMAND ${NOELLE_LOOP_PARITY_OPT}
              -basic-aa -mem2reg -scalar-evolution -loops
              -loop-simplify -lcssa -domtree -postdomtree
              -S ${pre_bc} -o ${ll_file}
      DEPENDS ${src_file}
      COMMENT "Generating LLVM IR for NOELLE parity fixture ${case_name}"
      VERBATIM)
endforeach()

add_custom_target(noelle_loop_parity_ll_fixtures DEPENDS ${NOELLE_LOOP_PARITY_LL_OUTPUTS})
list(APPEND analysis_test_dependencies noelle_loop_parity_ll_fixtures)
list(APPEND analysis_test_definitions
  LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR="${NOELLE_LOOP_PARITY_SRC_DIR}"
  LOTUS_NOELLE_LOOP_PARITY_LL_DIR="${NOELLE_LOOP_PARITY_LL_DIR}")
endif()

# TypeHierarchy fixture generation
if(type_hierarchy_selected)
set(TYPE_HIERARCHY_SRC_DIR
    ${CMAKE_SOURCE_DIR}/tests/regress/Analysis/TypeHierarchy)
set(TYPE_HIERARCHY_LL_DIR
    ${CMAKE_BINARY_DIR}/tests/unit/type_hierarchy_ll)

file(MAKE_DIRECTORY ${TYPE_HIERARCHY_LL_DIR})

if(APPLE AND EXISTS /opt/homebrew/opt/llvm@14/bin/clang++)
  set(TYPE_HIERARCHY_CLANGXX /opt/homebrew/opt/llvm@14/bin/clang++)
elseif(APPLE AND EXISTS /usr/local/opt/llvm@14/bin/clang++)
  set(TYPE_HIERARCHY_CLANGXX /usr/local/opt/llvm@14/bin/clang++)
elseif(EXISTS "${LOTUS_LLVM_TOOLS_BINARY_DIR}/clang++")
  set(TYPE_HIERARCHY_CLANGXX "${LOTUS_LLVM_TOOLS_BINARY_DIR}/clang++")
elseif(EXISTS "${LOTUS_LLVM_TOOLS_BINARY_DIR}/clang++-14")
  set(TYPE_HIERARCHY_CLANGXX "${LOTUS_LLVM_TOOLS_BINARY_DIR}/clang++-14")
elseif(EXISTS "${LLVM_TOOLS_BINARY_DIR}/clang++")
  set(TYPE_HIERARCHY_CLANGXX "${LLVM_TOOLS_BINARY_DIR}/clang++")
else()
  find_program(TYPE_HIERARCHY_CLANGXX
               NAMES clang++-14 clang++
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR})
  if(NOT TYPE_HIERARCHY_CLANGXX)
    set(TYPE_HIERARCHY_CLANGXX ${CMAKE_CXX_COMPILER})
  endif()
endif()

if(APPLE)
  execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE TYPE_HIERARCHY_SYSROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(TYPE_HIERARCHY_CLANGXX_EXTRA_ARGS -isysroot ${TYPE_HIERARCHY_SYSROOT})
  if(EXISTS /opt/homebrew/opt/llvm@14/include/c++/v1 AND NOT TYPE_HIERARCHY_CLANGXX MATCHES "llvm@14")
    list(APPEND TYPE_HIERARCHY_CLANGXX_EXTRA_ARGS -nostdinc++ -isystem /opt/homebrew/opt/llvm@14/include/c++/v1)
  endif()
else()
  set(TYPE_HIERARCHY_CLANGXX_EXTRA_ARGS)
endif()

set(TYPE_HIERARCHY_CASES
    type_hierarchy_1
    type_hierarchy_2
    type_hierarchy_3
    type_hierarchy_4
    type_hierarchy_5
    type_hierarchy_6
    type_hierarchy_7
    type_hierarchy_7_b
    type_hierarchy_8
    type_hierarchy_9
    type_hierarchy_10
    type_hierarchy_11
    type_hierarchy_12
    type_hierarchy_12_b
    type_hierarchy_12_c
    type_hierarchy_13
    type_hierarchy_14
    type_hierarchy_15
    type_hierarchy_16
    type_hierarchy_17
    type_hierarchy_18
    type_hierarchy_19
    type_hierarchy_20
    type_hierarchy_21
    type_hierarchy_22
    call_graph_cha_rta
    call_graph_vta
    call_graph_fnptr)

set(TYPE_HIERARCHY_LL_OUTPUTS)
foreach(case_name ${TYPE_HIERARCHY_CASES})
  set(src_file ${TYPE_HIERARCHY_SRC_DIR}/${case_name}.cpp)
  set(ll_file ${TYPE_HIERARCHY_LL_DIR}/${case_name}_cpp_dbg.ll)
  list(APPEND TYPE_HIERARCHY_LL_OUTPUTS ${ll_file})
  add_custom_command(
      OUTPUT ${ll_file}
      COMMAND ${TYPE_HIERARCHY_CLANGXX}
              ${TYPE_HIERARCHY_CLANGXX_EXTRA_ARGS}
              -std=c++17 -S -emit-llvm -g -O0
              ${src_file} -o ${ll_file}
      DEPENDS ${src_file}
      COMMENT "Generating debug LLVM IR for ${case_name}"
      VERBATIM)
endforeach()

add_custom_target(type_hierarchy_ll_fixtures DEPENDS ${TYPE_HIERARCHY_LL_OUTPUTS})
list(APPEND analysis_test_dependencies type_hierarchy_ll_fixtures)
list(APPEND analysis_test_definitions
  LOTUS_TYPE_HIERARCHY_LL_DIR="${TYPE_HIERARCHY_LL_DIR}")
endif()

lotus_add_test_group(analysis cfg CFG analysis_tests LINK_LIBS CanaryCFG CanaryICFG)
lotus_add_test_group(analysis controldependence ControlDependence analysis_tests
  LINK_LIBS CanaryICFGControlDependence CanaryICFG)
lotus_add_test_group(analysis debuginfo DebugInfo analysis_tests LINK_LIBS CanaryDebugInfo)
lotus_add_test_group(analysis multiplicity Multiplicity analysis_tests LINK_LIBS CanaryMultiplicity)
lotus_add_test_group(analysis nullpointer NullPointer analysis_tests
  LINK_LIBS CanaryNullPointer CanaryDyckAA)
lotus_add_test_group(analysis parametersummary ParameterSummary analysis_tests
  LINK_LIBS CanaryParameterSummary)
lotus_add_test_group(analysis profile Profile analysis_tests LINK_LIBS CanaryProfile)
lotus_add_test_group(analysis purity Purity analysis_tests
  LINK_LIBS CanaryPurityAnalysis CanaryShadowMemSSA AliasSpecManager LLVMAnalysis)
lotus_add_test_group(analysis sccp SCCP analysis_tests LINK_LIBS CanarySCCP)

if(type_hierarchy_selected)
lotus_collect_test_sources(analysis_tests
  TypeHierarchy/DIBasedTypeHierarchyTest.cpp
  TypeHierarchy/DIBasedTypeHierarchyTransitiveReachabilityTest.cpp
  TypeHierarchy/DIBasedTypeHierarchySerializationTest.cpp
  TypeHierarchy/LLVMVFTableTest.cpp
  CallGraph/CallGraphAlgorithmsTest.cpp)
set_property(GLOBAL APPEND PROPERTY LOTUS_TEST_SUITE_analysis_tests_LINK_LIBS
  CanaryCallGraph CanaryTypeHierarchy LLVMAnalysis)
endif()
if(loop_selected)
lotus_collect_test_sources(analysis_tests
  Loop/LoopAnalysisFoundationTest.cpp
  Loop/LoopDependenceGraphTest.cpp
  Loop/LoopLDGBuilderTest.cpp
  Loop/LoopScalarAnalysisTest.cpp
  Loop/LoopEnvironmentAndCarriedDepsTest.cpp
  Loop/LoopIterationSpaceTest.cpp
  Loop/LoopClassificationTest.cpp
  Loop/LoopNestingGraphTest.cpp
  Loop/LoopParityIVTest.cpp
  Loop/LoopParityIterationSpaceTest.cpp
  Loop/LoopParitySCCDAGAttrsTest.cpp)
set_property(GLOBAL APPEND PROPERTY LOTUS_TEST_SUITE_analysis_tests_LINK_LIBS
  CanaryLoopAnalysis CanaryPDG LLVMAnalysis LLVMTransformUtils LLVMPasses)
endif()

get_property(analysis_test_libs GLOBAL PROPERTY LOTUS_TEST_SUITE_analysis_tests_LINK_LIBS)
list(REMOVE_DUPLICATES analysis_test_libs)
add_lotus_collected_test_suite(analysis_tests
  LINK_LIBS ${analysis_test_libs}
  INTEGRATION_TEST_REGEX
    "^analysis_tests\\.(DIBasedTypeHierarchyTest|CallGraphAlgorithmsTest|LoopParity[^.]+)\\."
  COMPILE_DEFINITIONS ${analysis_test_definitions}
  DEPENDS ${analysis_test_dependencies})
