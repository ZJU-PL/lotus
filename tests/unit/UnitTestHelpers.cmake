set(LOTUS_UNIT_TEST_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(LOTUS_UNIT_TEST_COMMON_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../include
    ${LLVM_INCLUDE_DIRS})

# Non-template LLVM test helpers are compiled once for all suites.
add_library(lotus_test_llvm_helpers STATIC EXCLUDE_FROM_ALL
    TestUtils/LLVMHelpers.cpp)
target_include_directories(lotus_test_llvm_helpers PUBLIC
    ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(lotus_test_llvm_helpers PUBLIC
    GTest::gtest LLVMAsmParser LLVMIRReader)

add_library(lotus_test_utils INTERFACE)
target_include_directories(lotus_test_utils INTERFACE
    ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(lotus_test_utils INTERFACE
    lotus_test_llvm_helpers
    GTest::gtest_main)

add_library(lotus_test_harness_utils INTERFACE)
target_include_directories(lotus_test_harness_utils INTERFACE
    ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(lotus_test_harness_utils INTERFACE
    lotus_test_llvm_helpers)

include(GoogleTest)

# Child directories declare sources; a parent links one cohesive executable.
function(lotus_collect_test_sources suite_name)
    foreach(source_file IN LISTS ARGN)
        if(IS_ABSOLUTE "${source_file}")
            set(source "${source_file}")
        else()
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
        endif()
        set_property(GLOBAL APPEND PROPERTY
            LOTUS_TEST_SUITE_${suite_name}_SOURCES "${source}")
    endforeach()
endfunction()

# Select a child source manifest and record only its required libraries.
function(lotus_add_test_group subsystem group directory suite_name)
    cmake_parse_arguments(LOTUS_GROUP "" "" "LINK_LIBS" ${ARGN})
    lotus_test_group_enabled(${subsystem} ${group} selected)
    if(selected)
        add_subdirectory(${directory})
        set_property(GLOBAL APPEND PROPERTY
            LOTUS_TEST_SUITE_${suite_name}_LINK_LIBS ${LOTUS_GROUP_LINK_LIBS})
    endif()
endfunction()

function(add_lotus_collected_test_suite suite_name)
    get_property(sources GLOBAL PROPERTY
        LOTUS_TEST_SUITE_${suite_name}_SOURCES)
    if(NOT sources)
        message(FATAL_ERROR
            "add_lotus_collected_test_suite(${suite_name}) has no sources")
    endif()
    add_lotus_test_suite(${suite_name} SOURCES ${sources} ${ARGN})
endfunction()

function(add_lotus_test_suite test_name)
    cmake_parse_arguments(LOTUS_SUITE "CUSTOM_MAIN"
        "TIMEOUT;TEST_KIND;COMPONENT_TEST_REGEX;INTEGRATION_TEST_REGEX"
        "SOURCES;LINK_LIBS;INCLUDE_DIRS;COMPILE_DEFINITIONS;DEPENDS;LABELS"
        ${ARGN})
    if(NOT LOTUS_SUITE_SOURCES)
        message(FATAL_ERROR
            "add_lotus_test_suite(${test_name}) requires at least one source")
    endif()

    add_executable(${test_name} ${LOTUS_SUITE_SOURCES})
    set_property(GLOBAL APPEND PROPERTY
        LOTUS_UNIT_TEST_EXECUTABLES "${test_name}")
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES}
        ${LOTUS_SUITE_INCLUDE_DIRS})
    target_compile_definitions(${test_name} PRIVATE
        LOTUS_GTEST_NO_MAIN
        ${LOTUS_SUITE_COMPILE_DEFINITIONS})
    if(LOTUS_SUITE_CUSTOM_MAIN)
        set(test_support lotus_test_harness_utils)
    else()
        set(test_support lotus_test_utils)
    endif()
    target_link_libraries(${test_name} PRIVATE
        ${test_support}
        ${LOTUS_SUITE_LINK_LIBS})
    if(LOTUS_SUITE_DEPENDS)
        add_dependencies(${test_name} ${LOTUS_SUITE_DEPENDS})
    endif()

    if(LOTUS_SUITE_TIMEOUT)
        set(test_timeout ${LOTUS_SUITE_TIMEOUT})
    else()
        set(test_timeout ${LOTUS_UNIT_TEST_TIMEOUT})
    endif()
    if(LOTUS_SUITE_TEST_KIND)
        string(TOLOWER "${LOTUS_SUITE_TEST_KIND}" test_kind)
    else()
        set(test_kind unit)
    endif()
    if(NOT test_kind MATCHES "^(unit|component|integration)$")
        message(FATAL_ERROR
            "add_lotus_test_suite(${test_name}) has invalid TEST_KIND "
            "'${LOTUS_SUITE_TEST_KIND}'")
    endif()

    file(RELATIVE_PATH relative_dir
        ${LOTUS_UNIT_TEST_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
    if(relative_dir STREQUAL "")
        set(subsystem unit)
    else()
        string(REGEX REPLACE "/.*$" "" subsystem "${relative_dir}")
        string(TOLOWER "${subsystem}" subsystem)
    endif()

    set(labels lotus ${subsystem} ${LOTUS_SUITE_LABELS})
    # CTest accumulates LABELS assignments. For mixed suites the post-discovery
    # script must be the sole owner of the execution-kind label.
    if(NOT LOTUS_SUITE_COMPONENT_TEST_REGEX AND
       NOT LOTUS_SUITE_INTEGRATION_TEST_REGEX)
        list(APPEND labels ${test_kind})
    endif()
    list(REMOVE_DUPLICATES labels)
    string(REPLACE ";" "\\;" labels_property "${labels}")
    # DISCOVERY_MODE PRE_TEST defers enumeration until ctest time instead of
    # running the test binary after every build. It requires CMake 3.18,
    # which is the project minimum. An explicit
    # CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE set by the user is respected.
    set(_lotus_gtest_discovery_args)
    if(NOT DEFINED CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE)
        list(APPEND _lotus_gtest_discovery_args DISCOVERY_MODE PRE_TEST)
    endif()
    gtest_discover_tests(${test_name}
        TEST_PREFIX "${test_name}."
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DISCOVERY_TIMEOUT ${LOTUS_TEST_DISCOVERY_TIMEOUT}
        ${_lotus_gtest_discovery_args}
        PROPERTIES
            TIMEOUT ${test_timeout}
            LABELS "${labels_property}")
    unset(_lotus_gtest_discovery_args)

    # CTest includes this after GTest discovery, so individual cases can carry
    # their actual dependency kind while sharing one linked subsystem binary.
    if(LOTUS_SUITE_COMPONENT_TEST_REGEX OR LOTUS_SUITE_INTEGRATION_TEST_REGEX)
        set(classification_script
            ${CMAKE_CURRENT_BINARY_DIR}/${test_name}_kinds.cmake)
        configure_file(
            ${LOTUS_UNIT_TEST_ROOT}/../cmake/ClassifyGTests.cmake.in
            ${classification_script} @ONLY)
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
            ${classification_script})
    endif()
endfunction()

function(add_lotus_targeted_test test_name source_file)
    cmake_parse_arguments(LOTUS_TEST "" "" "LINK_LIBS;INCLUDE_DIRS" ${ARGN})
    if(IS_ABSOLUTE "${source_file}")
        set(source "${source_file}")
    else()
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
    endif()
    add_lotus_test_suite(${test_name}
        SOURCES ${source}
        LINK_LIBS ${LOTUS_TEST_LINK_LIBS}
        INCLUDE_DIRS ${LOTUS_TEST_INCLUDE_DIRS})
endfunction()

function(add_lotus_concurrency_test_suite test_name)
    add_lotus_test_suite(${test_name}
        SOURCES ${ARGN}
        LINK_LIBS Concurrency CanaryParallel)
endfunction()
