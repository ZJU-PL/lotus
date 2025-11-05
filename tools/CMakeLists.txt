
add_subdirectory(alias)
add_subdirectory(checker)
add_subdirectory(pdg-query)
add_subdirectory(lsp)


# Verifier
## CLAM tools (clam, clam-diff, clam-pp)
if(ENABLE_CLAM)
    add_subdirectory(verifier/clam)
endif()

## Seahorn tools
#if(ENABLE_SEAHORN)
#    add_subdirectory(verifier/seahorn)
#endif()

## horn-ice
option(BUILD_HORN_ICE "Build ICE learning for CHC and Boogie" OFF)
if(BUILD_HORN_ICE)
    add_subdirectory(verifier/horn-ice)
endif()

# CFL reachability
option(BUILD_CSR "Build indexing context-sensitive reachability solver" OFF)
if(BUILD_CSR)
    add_subdirectory(cfl/csr)
endif()

option(BUILD_GRASPAN "Build graspan" OFF)
if(BUILD_GRASPAN)
    add_subdirectory(cfl/graspan)
endif()

# Dynamic alias analysis
option(BUILD_DYNAA "Build dynamic alias analyses" OFF)
if(BUILD_DYNAA)
    add_subdirectory(alias/dynaa)
endif()

# Optional SMT tools
option(BUILD_OWL "Build Owl SMT solver" OFF)

if(BUILD_OWL)
    add_subdirectory(owl)
endif()
