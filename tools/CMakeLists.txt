
add_subdirectory(alias)
add_subdirectory(checker)
add_subdirectory(pdg-query)
add_subdirectory(mcp)
add_subdirectory(verifier)
add_subdirectory(cfl)

# Optional SMT tools
option(BUILD_OWL "Build Owl SMT solver" OFF)

if(BUILD_OWL)
    add_subdirectory(owl)
endif()
