add_library(CanaryADT STATIC
    UnionFind.cpp
    TreeStream.cpp
    WrappedInterval.cpp
    HierarchicalClusterer.cpp
)
target_link_libraries(CanaryADT PUBLIC CanaryFastCluster)
