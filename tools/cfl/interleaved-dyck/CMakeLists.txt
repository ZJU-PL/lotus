add_executable(lotus-cfl-interleaved-dyck
    main.cpp
    affine-spds/lotus-cfl-interleaved-dyck-affine-spds.cpp
    spds/lotus-cfl-interleaved-dyck-spds.cpp
    unary/lotus-cfl-interleaved-dyck-unary.cpp
    staged-bounds/lotus-cfl-interleaved-dyck-staged-bounds.cpp
    mcfl/lotus-cfl-interleaved-dyck-mcfl.cpp
    lcl/lotus-cfl-interleaved-dyck-lcl.cpp
    ilp/lotus-cfl-interleaved-dyck-ilp.cpp
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterleavedDyck/GraphReduction/GraphAux.cpp
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterleavedDyck/GraphReduction/DkMerge.cpp)

target_link_libraries(lotus-cfl-interleaved-dyck PRIVATE
    CanaryInterleavedDyckAffineSPDS
    CanaryInterleavedDyckSPDS
    CanaryInterleavedDyckUnary
    CanaryInterleavedDyckStagedBounds
    CanaryInterleavedDyckMCFL
    CanaryInterleavedDyckLCL
    CanaryInterleavedDyckILP
    CanaryInterleavedDyckGraphReduction
    CanaryADT)

configure_file(
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterleavedDyck/GraphReduction/graph_simp.py
    ${CMAKE_BINARY_DIR}/bin/lotus-cfl-interleaved-dyck-graph-reduction.py
    COPYONLY)

add_custom_target(lotus-cfl-interleaved-dyck-graph-reduction
    DEPENDS lotus-cfl-interleaved-dyck)
