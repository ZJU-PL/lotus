# Graspan - CFL Reachability Tool
#
# Disk-based parallel graph system for computing dynamic transitive closures

# The graspan executable - only contains the main entry point
add_executable(graspan graspan.cpp)

# Include the Graspan library headers
target_include_directories(graspan PRIVATE
    ${CMAKE_SOURCE_DIR}/include/CFL/Graspan
    ${Boost_INCLUDE_DIRS}
)

# Link against the Graspan library and Boost dependencies
target_link_libraries(graspan
    Graspan
    pthread
)

# Conditionally link Boost libraries if available as targets
if(TARGET Boost::system)
    target_link_libraries(graspan Boost::system)
endif()

if(TARGET Boost::thread)
    target_link_libraries(graspan Boost::thread)
endif()
