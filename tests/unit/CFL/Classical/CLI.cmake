# CLI and regression coverage for lib/CFL/Classical
# (lotus-cfl-solve, lotus-cfl-alias, and lotus-cfl-vf drivers).



if(TARGET lotus-cfl-solve)
    foreach(classical_backend
            sparse-set sparse-bitvector graspan sqid pearl skewed cat iea iea-ocr
            transitive-closure pocr hpocr
            focr endpoint-quotient cert)
        add_test(NAME classical_cfl_cli_${classical_backend}
            COMMAND $<TARGET_FILE:lotus-cfl-solve>
                --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.grammar
                --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.txt
                --solver ${classical_backend}
                --json-stats)
        set_tests_properties(classical_cfl_cli_${classical_backend} PROPERTIES
            PASS_REGULAR_EXPRESSION "\"relation_edges\":64")

        if(NOT classical_backend STREQUAL "cert")
            add_test(NAME classical_cfl_pocr_format_${classical_backend}
                COMMAND $<TARGET_FILE:lotus-cfl-solve>
                    --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.grammar
                    --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.graph
                    --solver ${classical_backend}
                    --unidirectional
                    --json-stats)
            set_tests_properties(
                classical_cfl_pocr_format_${classical_backend} PROPERTIES
                PASS_REGULAR_EXPRESSION "\"relation_edges\":8")
        endif()
    endforeach()

    add_test(NAME classical_cfl_cli_stg
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.txt
            --solver stg
            --stg-spec ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/stg-chain.json
            --json-stats)
    set_tests_properties(classical_cfl_cli_stg PROPERTIES
        PASS_REGULAR_EXPRESSION
        "\"relation_edges\":64.*\"stg_phase_r_edges\":55")

    add_test(NAME classical_cfl_cli_infers_attributes
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.txt
            --solver transitive-closure
            --json-stats)
    set_tests_properties(classical_cfl_cli_infers_attributes PROPERTIES
        PASS_REGULAR_EXPRESSION "\"relation_edges\":3")

    add_test(NAME classical_cfl_cli_transitive_stats
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.txt
            --solver transitive-closure
            --json-stats)
    set_tests_properties(classical_cfl_cli_transitive_stats PROPERTIES
        PASS_REGULAR_EXPRESSION "\"transitive_instances\":1")

    add_test(NAME classical_cfl_cli_validate_only
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.txt
            --validate-only)
    set_tests_properties(classical_cfl_cli_validate_only PROPERTIES
        PASS_REGULAR_EXPRESSION "validation=ok")

    add_test(NAME classical_cfl_cli_graph_simplification
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.graph
            --solver pocr
            --simplification-flavor alias
            --simplify-graph
            --json-stats)
    set_tests_properties(classical_cfl_cli_graph_simplification PROPERTIES
        PASS_REGULAR_EXPRESSION "\"simplified_nodes\":1")

    add_test(NAME classical_cfl_cli_count_symbols
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.graph
            --solver pocr
            --unidirectional
            --json-stats)
    set_tests_properties(classical_cfl_cli_count_symbols PROPERTIES
        PASS_REGULAR_EXPRESSION "\"count_edges\":3")

    add_test(NAME classical_cfl_cli_projected_metadata
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar
            ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/projected-metadata.grammar
            --graph
            ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/projected-metadata.graph
            --solver skewed
            --result-scope count
            --json-stats)
    set_tests_properties(classical_cfl_cli_projected_metadata PROPERTIES
        PASS_REGULAR_EXPRESSION
        "\"result_scope\":\"count\".*\"count_edges\":1.*\"skewed_propagating_symbols\":1")

    foreach(pocr_client aa vf)
        if(pocr_client STREQUAL "aa")
            set(expected_rewritten_start_edges 8)
        else()
            set(expected_rewritten_start_edges 6)
        endif()
        foreach(grammar_variant "" "-rewritten")
            foreach(epoch_backend sparse-bitvector graspan)
                string(REPLACE "-" "_" variant_name "${grammar_variant}")
                string(REPLACE "-" "_" backend_name "${epoch_backend}")
                add_test(
                    NAME classical_cfl_grammar_${pocr_client}${variant_name}_${backend_name}
                    COMMAND $<TARGET_FILE:lotus-cfl-solve>
                        --grammar
                        ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr-${pocr_client}${grammar_variant}.grammar
                        --graph
                        ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr-${pocr_client}.graph
                        --solver ${epoch_backend}
                        --json-stats)
                set_tests_properties(
                    classical_cfl_grammar_${pocr_client}${variant_name}_${backend_name}
                    PROPERTIES PASS_REGULAR_EXPRESSION
                    "\"start_edges\":${expected_rewritten_start_edges}")
            endforeach()
        endforeach()
    endforeach()
endif()

if(TARGET lotus-cfl-alias)
    add_test(NAME classical_cfl_alias_indirect_call
        COMMAND $<TARGET_FILE:lotus-cfl-alias>
            --solver sparse-bitvector
            --encoding pag
            --json-stats
            ${CMAKE_SOURCE_DIR}/tests/regress/Alias/PTA/funptr-simple.ll)
    set_tests_properties(classical_cfl_alias_indirect_call PROPERTIES
        PASS_REGULAR_EXPRESSION "\"callgraph_rounds\":2")

    set(classical_alias_export_graph
        ${CMAKE_BINARY_DIR}/tests/classical-alias-export.graph)
    set(classical_alias_export_grammar
        ${CMAKE_BINARY_DIR}/tests/classical-alias-export.grammar)
    add_test(NAME classical_cfl_alias_export
        COMMAND $<TARGET_FILE:lotus-cfl-alias>
            --solver sparse-bitvector
            --encoding cfl-peg
            --dump-cfl-graph ${classical_alias_export_graph}
            --dump-cfl-grammar ${classical_alias_export_grammar}
            --json-stats
            ${CMAKE_SOURCE_DIR}/tests/regress/Alias/PTA/funptr-simple.ll)
    set_tests_properties(classical_cfl_alias_export PROPERTIES
        PASS_REGULAR_EXPRESSION "\"encoding\":\"cfl-peg\"")
    add_test(NAME classical_cfl_alias_export_roundtrip
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${classical_alias_export_grammar}
            --graph ${classical_alias_export_graph}
            --solver sparse-bitvector
            --json-stats)
    set_tests_properties(classical_cfl_alias_export_roundtrip PROPERTIES
        DEPENDS classical_cfl_alias_export
        PASS_REGULAR_EXPRESSION "\"relation_edges\":228")

    set(classical_alias_annotation_cases
        funptr-simple
        array-varIdx
        struct-assignment-direct
        heap-indirect
        constraint-cycle-field
        array-varIdx2
        struct-nested-2-layers
        field-ptr-arith-constIdx
        ptr-dereference3
        heap-linkedlist
        global-initializer
        struct-twoflds)
    foreach(alias_case IN LISTS classical_alias_annotation_cases)
        add_test(NAME classical_cfl_alias_annotation_${alias_case}
            COMMAND $<TARGET_FILE:lotus-cfl-alias>
                --solver sparse-bitvector
                --encoding pag
                --check-annotations
                ${CMAKE_SOURCE_DIR}/tests/regress/Alias/PTA/${alias_case}.ll)
        set_tests_properties(classical_cfl_alias_annotation_${alias_case}
            PROPERTIES PASS_REGULAR_EXPRESSION "annotation=pass")
    endforeach()
endif()

if(TARGET lotus-cfl-vf)
    foreach(classical_backend
            sparse-set sparse-bitvector graspan sqid pearl skewed cat iea iea-ocr
            transitive-closure pocr hpocr
            focr endpoint-quotient cert)
        add_test(NAME classical_cfl_vf_${classical_backend}
            COMMAND $<TARGET_FILE:lotus-cfl-vf>
                --solver ${classical_backend}
                --query main::seed,main::result
                --json-stats
                ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/value-flow.ll)
        set_tests_properties(classical_cfl_vf_${classical_backend} PROPERTIES
            PASS_REGULAR_EXPRESSION "flow=yes")
    endforeach()

    set(classical_vf_export_graph
        ${CMAKE_BINARY_DIR}/tests/classical-vf-export.graph)
    set(classical_vf_export_grammar
        ${CMAKE_BINARY_DIR}/tests/classical-vf-export.grammar)
    add_test(NAME classical_cfl_vf_export
        COMMAND $<TARGET_FILE:lotus-cfl-vf>
            --encoding classical-cfl
            --solver sparse-bitvector
            --dump-cfl-graph ${classical_vf_export_graph}
            --dump-cfl-grammar ${classical_vf_export_grammar}
            --json-stats
            ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/value-flow.ll)
    set_tests_properties(classical_cfl_vf_export PROPERTIES
        PASS_REGULAR_EXPRESSION "\"encoding\":\"classical-cfl\"")
    add_test(NAME classical_cfl_vf_export_roundtrip
        COMMAND $<TARGET_FILE:lotus-cfl-solve>
            --grammar ${classical_vf_export_grammar}
            --graph ${classical_vf_export_graph}
            --solver sparse-bitvector
            --json-stats)
    set_tests_properties(classical_cfl_vf_export_roundtrip PROPERTIES
        DEPENDS classical_cfl_vf_export
        PASS_REGULAR_EXPRESSION "\"relation_edges\":29")
endif()

if(TARGET lotus-cfl-subcubic-aa)
    foreach(subcubic_case figure4 modes input_validation options json_names)
        add_test(NAME classical_cfl_cli_subcubic_${subcubic_case}
            COMMAND ${CMAKE_COMMAND}
                -DBINARY=$<TARGET_FILE:lotus-cfl-subcubic-aa>
                -DCASE=${subcubic_case}
                -DFIGURE4=${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/subcubic-aa-figure4.graph
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/subcubic-cli/${subcubic_case}
                -P ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/RunSubcubicAA.cmake)
    endforeach()
endif()

get_property(cfl_registered_tests DIRECTORY PROPERTY TESTS)
foreach(cfl_registered_test IN LISTS cfl_registered_tests)
    if(cfl_registered_test MATCHES
       "^classical_cfl_(cli|alias|vf|grammar)")
        set_tests_properties(${cfl_registered_test} PROPERTIES
            LABELS "lotus;integration;cfl;cli"
            TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    endif()
endforeach()
