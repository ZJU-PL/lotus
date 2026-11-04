# Call from a regression directory after registering its CLI cases. Apply
# defaults to every case, including error paths, without losing extra labels.
function(lotus_label_regression_tests subsystem)
    get_property(regression_tests DIRECTORY PROPERTY TESTS)
    foreach(test_case IN LISTS regression_tests)
        get_test_property(${test_case} LABELS labels)
        if(labels STREQUAL "NOTFOUND")
            set(labels)
        endif()
        list(REMOVE_ITEM labels unit component)
        list(APPEND labels lotus integration ${subsystem} cli)
        list(REMOVE_DUPLICATES labels)
        set_tests_properties(${test_case} PROPERTIES
            LABELS "${labels}"
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        get_test_property(${test_case} TIMEOUT timeout)
        if(timeout STREQUAL "NOTFOUND")
            set_tests_properties(${test_case} PROPERTIES
                TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT})
        endif()
    endforeach()
endfunction()
