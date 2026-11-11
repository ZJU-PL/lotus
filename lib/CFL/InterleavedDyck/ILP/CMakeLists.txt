add_library(CanaryInterleavedDyckILP STATIC
    Solver.cpp
    Backend.cpp
    Z3Backend.cpp)
target_include_directories(CanaryInterleavedDyckILP PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include)
target_link_libraries(CanaryInterleavedDyckILP
    PUBLIC
        CanaryInterleavedDyckCore
        CanaryInterleavedDyckLCL
        Z3::z3)

if(LOTUS_ENABLE_GUROBI)
    include(${CMAKE_SOURCE_DIR}/cmake/FindGurobi.cmake)
    if(NOT Gurobi_FOUND)
        message(FATAL_ERROR
            "LOTUS_ENABLE_GUROBI=ON but Gurobi was not found; set GUROBI_HOME")
    endif()
    target_sources(CanaryInterleavedDyckILP PRIVATE GurobiBackend.cpp)
    target_compile_definitions(CanaryInterleavedDyckILP PUBLIC LOTUS_HAVE_GUROBI=1)
    target_link_libraries(CanaryInterleavedDyckILP PRIVATE Gurobi::Gurobi)
endif()
