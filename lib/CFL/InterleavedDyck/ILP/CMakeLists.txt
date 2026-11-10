add_library(CanaryInterleavedDyckILP STATIC Solver.cpp)
target_include_directories(CanaryInterleavedDyckILP PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include)
target_link_libraries(CanaryInterleavedDyckILP
    PUBLIC
        CanaryInterleavedDyckCore
        CanaryInterleavedDyckLCL
        Z3::z3)
