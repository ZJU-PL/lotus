cmake_minimum_required(VERSION 3.10)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hice-dt/include
)

set(LIB_SOURCES
  boogie_io.cpp
  decision_tree.cpp
  output_visitor.cpp
  pretty_print_visitor.cpp
  horn_constraint.cpp
  horn_solver.cpp
  datapoint.cpp
  job.cpp
)

# Create object library for better organization
add_library(chc_verifier_obj OBJECT ${LIB_SOURCES})
set_target_properties(chc_verifier_obj PROPERTIES 
    POSITION_INDEPENDENT_CODE ON
    COMPILE_FLAGS "-frtti"
)

# Create the static library from object files
add_library(chc_verifier_lib STATIC $<TARGET_OBJECTS:chc_verifier_obj>)

# Create the executable
add_executable(chc_verifier chc_verifier.cpp)
target_link_libraries(chc_verifier chc_verifier_lib ${Z3_LIBRARIES} pthread)

# Enable RTTI for dynamic_cast
set_target_properties(chc_verifier PROPERTIES COMPILE_FLAGS "-frtti")

# Add OpenMP flags if available
#find_package(OpenMP)
#if(OpenMP_CXX_FOUND)
#    target_link_libraries(chc_verifier OpenMP::OpenMP_CXX)
#endif()

install(TARGETS chc_verifier DESTINATION bin) 