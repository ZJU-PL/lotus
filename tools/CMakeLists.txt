
add_subdirectory(alias)
add_subdirectory(checker)
add_subdirectory(verifier)

#add_subdirectory(pdg-query)  # not used for now
#add_subdirectory(mcp)        # not used for now

option(BUILD_CFL "Build CFL Reachability Solvers" OFF)
if(BUILD_CFL)
    add_subdirectory(cfl)
endif()

# Optional SMT tools
option(BUILD_OWL "Build Owl SMT solver" OFF)
if(BUILD_OWL)
    add_subdirectory(owl)
endif()
