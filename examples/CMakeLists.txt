# Examples CMakeLists.txt

set(EXAMPLES
  RapidJSONExample.cpp
  AndersenExample.cpp
  PDGExample.cpp
)

set(EXAMPLES_NAMES
  RapidJSONExample
  AndersenExample
  PDGExample
)

set(EXAMPLES_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

# Get LLVM components needed for examples that don't get LLVM through dependencies
llvm_map_components_to_libnames(llvm_libs irreader analysis)

# Add executables and include directories
foreach(idx RANGE 0 3)
  list(GET EXAMPLES_NAMES ${idx} example_name)
  list(GET EXAMPLES ${idx} example_file)
  add_executable(${example_name} ${example_file})
  target_include_directories(${example_name} PUBLIC ${EXAMPLES_INCLUDE_DIR})
endforeach()

# AndersenExample needs Andersen and LLVM libs
 target_link_libraries(AndersenExample PRIVATE Andersen ${llvm_libs})

# PDGExample needs CanaryPDG and LLVM libs
 target_link_libraries(PDGExample PRIVATE CanaryPDG ${llvm_libs})
