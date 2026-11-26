# Vendored fastcluster (Daniel Müllner, BSD). Used by the hierarchical
# object-ID clustering pass; kept as a separate target so its warnings do not
# leak into the rest of the tree.
add_library(CanaryFastCluster STATIC
    FastCluster.cpp
)
target_include_directories(CanaryFastCluster PUBLIC ${CMAKE_SOURCE_DIR}/include)
