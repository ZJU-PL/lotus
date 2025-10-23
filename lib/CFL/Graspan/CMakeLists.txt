# Graspan - CFL Reachability Library
# Disk-based parallel graph system for computing dynamic transitive closures

# Define macros to avoid boost::throw_exception issues and provide our own implementation
add_definitions(-DBOOST_NO_EXCEPTIONS -DBOOST_EXCEPTION_DISABLE)

# Source files for Graspan library (excluding the main entry point)
set(GRASPAN_LIB_SOURCES
    datastructures/context.cpp
    datastructures/DDM.cpp
    datastructures/loadedvertexinterval.cpp
    datastructures/loader.cpp
    datastructures/minset.cpp
    datastructures/partition.cpp
    datastructures/vertex.cpp
    datastructures/vit.cpp
    edgecomp/compute.cpp
    edgecomp/edgemerger.cpp
    edgecomp/engine.cpp
    edgecomp/grammar.cpp
    edgecomp/repart.cpp
    preproc/preproc.cpp
    preproc/preproc_new.cpp
    preproc/run_pre.cpp
    utilities/timer.cpp
    Library/Logger.cpp
)

# Create Graspan static library
add_library(Graspan STATIC ${GRASPAN_LIB_SOURCES})

# Include directories - headers are now in include/CFL/Graspan
#target_include_directories(Graspan PUBLIC
 #   ${CMAKE_SOURCE_DIR}/include/CFL/Graspan
#)
