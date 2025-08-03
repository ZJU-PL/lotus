add_subdirectory(canary)
add_subdirectory(kint)
add_subdirectory(seadsa)
add_subdirectory(fpa)
add_subdirectory(dynaa)
add_subdirectory(taint)
add_subdirectory(gvfa)
add_subdirectory(conftainter)

# Optional targets - OFF by default
option(BUILD_OWL "Build Owl SMT solver" OFF)
option(BUILD_HORN_ICE "Build ICE learning for CHC and Boogie" OFF)
option(BUILD_CSR "Build indexing context-sensitive reachability solver" OFF)

# Tool for solving SMT-LIB2 files
if(BUILD_OWL)
    add_subdirectory(owl)
endif()

# Tool for inductive invariant inference
if(BUILD_HORN_ICE)
    add_subdirectory(horn-ice)
endif()

# Tool for context-sensitive reachability
if(BUILD_CSR)
    add_subdirectory(csr)
endif() 
