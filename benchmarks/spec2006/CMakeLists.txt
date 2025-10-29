find_program(BASH_BIN bash)
if (NOT BASH_BIN)
    message(FATAL_ERROR "Can not find bash, stop regression testing")
endif()

file(GLOB RegressionScript regression.sh)
add_custom_target(regression
        COMMAND ${BASH_BIN} ${RegressionScript} ${CMAKE_BINARY_DIR}/bin/lotus
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS lotus
        SOURCES regression.sh
)