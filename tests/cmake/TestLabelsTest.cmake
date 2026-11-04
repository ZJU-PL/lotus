cmake_minimum_required(VERSION 3.18)

# Exercise the real CTest interpreter: LABELS assignments there accumulate,
# unlike configure-time CMake properties. A case must acquire its kind once.
function(check_labels default_kind component_regex integration_regex
         expected_unit expected_component expected_integration)
    set(fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/test-labels/${default_kind}")
    file(MAKE_DIRECTORY "${fixture_dir}")
    set(test_name fixture)
    set(test_kind "${default_kind}")
    set(labels "lotus;build")
    set(LOTUS_SUITE_COMPONENT_TEST_REGEX "${component_regex}")
    set(LOTUS_SUITE_INTEGRATION_TEST_REGEX "${integration_regex}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/ClassifyGTests.cmake.in"
        "${fixture_dir}/classify.cmake" @ONLY)

    set(registration "set(fixture_TESTS fixture.Unit fixture.Component fixture.Integration fixture.Both)\n")
    foreach(case Unit Component Integration Both)
        string(APPEND registration
            "add_test(fixture.${case} \"${CMAKE_COMMAND}\" -E true)\n"
            "set_tests_properties(fixture.${case} PROPERTIES LABELS \"lotus;build\")\n")
    endforeach()
    string(APPEND registration "include(\"${fixture_dir}/classify.cmake\")\n")
    file(WRITE "${fixture_dir}/CTestTestfile.cmake" "${registration}")

    foreach(label unit component integration lotus build)
        if(label STREQUAL "lotus" OR label STREQUAL "build")
            set(expected 4)
        else()
            set(expected "${expected_${label}}")
        endif()
        execute_process(
            COMMAND "${CTEST_COMMAND}" -N -L "^${label}$"
            WORKING_DIRECTORY "${fixture_dir}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(NOT result EQUAL 0 OR
           NOT output MATCHES "Total Tests: ${expected}([^0-9]|$)")
            message(FATAL_ERROR
                "${default_kind}/${label}: expected ${expected} cases: ${output}${error}")
        endif()
    endforeach()
endfunction()

check_labels(unit "\\.(Component|Both)$" "\\.(Integration|Both)$" 1 1 2)
check_labels(component "" "" 0 4 0)
check_labels(integration "" "" 0 0 4)
message(STATUS "Test label checks passed")
