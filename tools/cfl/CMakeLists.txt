# CFL tools umbrella
#
# Provides build toggles for CSR and GRASPAN CFL reachability tools and
# delegates to their respective subdirectories.
#
# Usage from parent:
#   add_subdirectory(cfl)

option(BUILD_CSR "Build indexing context-sensitive reachability solver" OFF)
if(BUILD_CSR)
    add_subdirectory(csr)
endif()

option(BUILD_GRASPAN "Build graspan" OFF)
if(BUILD_GRASPAN)
    add_subdirectory(graspan)
endif()


