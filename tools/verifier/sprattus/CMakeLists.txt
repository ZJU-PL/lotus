# Sprattus analysis tool

add_executable(sprattus spranalyze.cpp)

llvm_map_components_to_libnames(sprattus_tool_llvm_libs
    core irreader bitreader)

target_link_libraries(sprattus PRIVATE
    SprattusAnalysis
    ${sprattus_tool_llvm_libs}
    ${Z3_LIBRARIES}
)

# Ensure all registration objects from the static lib are linked (avoid dead stripping)
if(APPLE)
  # On macOS, force loading of all object files from static archives
  target_link_options(sprattus PRIVATE -Wl,-all_load)
endif()

target_include_directories(sprattus PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
    ${Z3_INCLUDE_DIRS}
)

install(TARGETS sprattus DESTINATION bin)

