# Match the same subsystem/group selection as the GTest manifests.
lotus_test_group_enabled(ir pdg pdg_selected)
if(pdg_selected AND TARGET lotus-ir-pdg-query)
    add_subdirectory(IR/PDG)
endif()

lotus_test_group_enabled(alias bootstrapaa bootstrap_selected)
if(bootstrap_selected)
    add_subdirectory(Alias/BootstrapAA)
endif()

lotus_test_group_enabled(alias sparsepta sparsepta_selected)
if(sparsepta_selected AND TARGET lotus-alias-fspta)
    add_subdirectory(Alias/SparsePTA)
endif()

if("checker" IN_LIST LOTUS_SELECTED_TEST_SUBSYSTEMS AND TARGET lotus-check)
    add_subdirectory(Checker/CLI)
endif()

if("cfl" IN_LIST LOTUS_SELECTED_TEST_ROOTS)
    add_subdirectory(CFL/Classical)
    add_subdirectory(CFL/DynamicDyck)
    add_subdirectory(CFL/InterleavedDyck)
endif()

lotus_test_group_enabled(dataflow apa apa_selected)
if(apa_selected)
    add_subdirectory(Dataflow/APA)
endif()
