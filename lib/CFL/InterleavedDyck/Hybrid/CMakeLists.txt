add_library(CanaryInterleavedDyckHybrid STATIC Solver.cpp)
target_include_directories(CanaryInterleavedDyckHybrid PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include)
target_link_libraries(CanaryInterleavedDyckHybrid PUBLIC
  CanaryInterleavedDyckCore
  CanaryInterleavedDyckSPDS
  CanaryInterleavedDyckAffineSPDS)
