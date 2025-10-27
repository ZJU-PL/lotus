
add_subdirectory(alias)
add_subdirectory(checker)
add_subdirectory(csr)
add_subdirectory(graspan)
add_subdirectory(pdg-query)

# CLAM tools (clam, clam-diff, clam-pp)
if(ENABLE_CLAM)
    add_subdirectory(clam)
endif()


option(BUILD_CSR "Build indexing context-sensitive reachability solver" OFF)
option(BUILD_DYNAA "Build dynamic alias analyses" OFF)

if(BUILD_DYNAA)
    add_subdirectory(alias/dynaa)
endif()

# Optional SMT and Verification tools
option(BUILD_OWL "Build Owl SMT solver" OFF)
option(BUILD_HORN_ICE "Build ICE learning for CHC and Boogie" OFF)

if(BUILD_OWL)
    add_subdirectory(owl)
endif()
if(BUILD_HORN_ICE)
    add_subdirectory(horn-ice)
endif()
