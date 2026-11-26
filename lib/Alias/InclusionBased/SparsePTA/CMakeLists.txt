add_library(SparsePTA STATIC
  Sparse/FlowSensitivePTA.cpp
  Parallel/ParallelFlowSensitivePTA.cpp
  Parallel/OrderedFlowSensitivePTA.cpp
  Versioned/VersionedFlowSensitivePTA.cpp)

target_link_libraries(SparsePTA
  PUBLIC
    SVFG
    CanaryAliasPtsSet)

find_package(Threads REQUIRED)
target_link_libraries(SparsePTA PRIVATE Threads::Threads)

add_library(Lotus::SparsePTA ALIAS SparsePTA)
add_library(FlowSensitivePTA ALIAS SparsePTA)
