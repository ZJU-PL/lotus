# CLI and regression coverage for lib/CFL/InterleavedDyck
# (lotus-cfl-interleaved-dyck driver plus tests/regress/CFL/InterleavedDyck).

include(${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/LCL/Tests.cmake)

include(${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/SPDS/Tests.cmake)
include(${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/AffineSPDS/Tests.cmake)

if(TARGET lotus-cfl-interleaved-dyck)
    foreach(engine lcl spds affine-spds unary staged-bounds mcfl)
        add_test(NAME interleaved_dyck_cli_${engine}_help
            COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck> ${engine} --help)
        set_tests_properties(interleaved_dyck_cli_${engine}_help PROPERTIES
            PASS_REGULAR_EXPRESSION "lotus-cfl-interleaved-dyck ${engine}"
            LABELS "lotus;integration;cfl;cli")
    endforeach()
    add_test(NAME interleaved_dyck_cli_engine_option
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            --engine spds --help)
    set_tests_properties(interleaved_dyck_cli_engine_option PROPERTIES
        PASS_REGULAR_EXPRESSION "lotus-cfl-interleaved-dyck spds"
        LABELS "lotus;integration;cfl;cli")
    add_test(NAME interleaved_dyck_cli_unary_smoke
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            unary --bidirect
            ${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/LCL/crossing.dot)
    set_tests_properties(interleaved_dyck_cli_unary_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "Algorithm: adaptive"
        LABELS "lotus;integration;cfl;cli")
    add_test(NAME interleaved_dyck_cli_mcfl_smoke
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            mcfl --dimension 2
            ${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/LCL/crossing.dot)
    set_tests_properties(interleaved_dyck_cli_mcfl_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "Reachable pairs"
        LABELS "lotus;integration;cfl;cli")
    add_test(NAME interleaved_dyck_graph_reduction_cli
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:lotus-cfl-interleaved-dyck>
            -DSCRIPT=${CMAKE_BINARY_DIR}/bin/lotus-cfl-interleaved-dyck-graph-reduction.py
            -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/LCL/crossing.dot
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/graph-reduction-cli
            -P ${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/GraphReduction/RunCLI.cmake)
    set_tests_properties(interleaved_dyck_graph_reduction_cli PROPERTIES
        LABELS "lotus;integration;cfl;cli")
endif()

if(TARGET lotus-cfl-interleaved-dyck)
    add_test(NAME interleaved_dyck_staged_bounds_cli_mutual_refinement
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            staged-bounds --method mutual-refinement
            ${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint/faketaobao.dot)
    set_tests_properties(
        interleaved_dyck_staged_bounds_cli_mutual_refinement PROPERTIES
        PASS_REGULAR_EXPRESSION "mutual-refinement upper bound: 328")
    add_test(NAME interleaved_dyck_staged_bounds_cli_taint_analysis
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            staged-bounds --analysis taint --method mutual-refinement --stage-stats
            ${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint/faketaobao.dot)
    set_tests_properties(
        interleaved_dyck_staged_bounds_cli_taint_analysis PROPERTIES
        PASS_REGULAR_EXPRESSION "mutual-refinement upper bound: 328")
    add_test(NAME interleaved_dyck_staged_bounds_cli_value_flow_analysis
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            staged-bounds --analysis value-flow --method mutual-refinement
            ${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/valueflow/xz.dot)
    set_tests_properties(
        interleaved_dyck_staged_bounds_cli_value_flow_analysis PROPERTIES
        PASS_REGULAR_EXPRESSION "mutual-refinement upper bound: 211")
endif()

if(TARGET lotus-cfl-interleaved-dyck)
    add_test(NAME interleaved_dyck_staged_bounds_cli_factorized
        COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
            staged-bounds --analysis value-flow --method mutual-refinement
            --factorized-tracing --stage-stats
            ${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/valueflow/xz.dot)
    set_tests_properties(interleaved_dyck_staged_bounds_cli_factorized PROPERTIES
        PASS_REGULAR_EXPRESSION "mutual-refinement upper bound: 211")
endif()

if(TARGET lotus-cfl-interleaved-dyck)
    foreach(method stronger-grammar on-demand)
        add_test(NAME interleaved_dyck_staged_bounds_cli_${method}_factorized
            COMMAND $<TARGET_FILE:lotus-cfl-interleaved-dyck>
                staged-bounds --method ${method}
                --factorized-tracing --stage-stats
                ${CMAKE_SOURCE_DIR}/tests/regress/CFL/InterleavedDyck/LCL/crossing.dot)
        set_tests_properties(interleaved_dyck_staged_bounds_cli_${method}_factorized PROPERTIES
            PASS_REGULAR_EXPRESSION "upper bound: 1")
    endforeach()
endif()

lotus_label_regression_tests(cfl)
