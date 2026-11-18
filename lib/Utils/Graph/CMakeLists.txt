# Unified Graphviz/DOT export for Lotus graphs.
add_library(CanaryGraphWriter STATIC
    DotWriter.cpp
)
target_include_directories(CanaryGraphWriter PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(CanaryGraphWriter PUBLIC LLVMSupport)
