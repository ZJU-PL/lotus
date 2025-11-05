set(SOURCES 
    boogie_io.cpp
    decision_tree.cpp
    output_visitor.cpp
    pretty_print_visitor.cpp
    horn_constraint.cpp
    horn_solver.cpp
    datapoint.cpp
    job.cpp
    json_visitor.cpp
    hice-dt.cpp
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Create object library first for better organization
add_library(hice-dt-lib OBJECT ${SOURCES})
set_target_properties(hice-dt-lib PROPERTIES 
    POSITION_INDEPENDENT_CODE ON
    COMPILE_FLAGS "-frtti"
)

# Create the executable from the object library
add_executable(hice-dt $<TARGET_OBJECTS:hice-dt-lib>)

# Enable RTTI for dynamic_cast
set_target_properties(hice-dt PROPERTIES COMPILE_FLAGS "-frtti")

# Link with Z3 library
target_link_libraries(hice-dt ${Z3_LIBRARIES})

install(TARGETS hice-dt DESTINATION bin) 