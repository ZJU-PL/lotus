cmake_minimum_required(VERSION 3.18)
project(LotusSVFDifferential LANGUAGES C CXX)

# Keep SVF out of the Lotus link graph: both frameworks define their own IR.
find_package(SVF CONFIG REQUIRED)
# Some in-tree SVF exports advertise an install-only include/SVF directory.
# Drop absent include paths locally without modifying the user's SVF build.
foreach(target SVF::SvfFlags SVF::SvfCore SVF::SvfLLVM)
  get_target_property(paths ${target} INTERFACE_INCLUDE_DIRECTORIES)
  set(existing_paths)
  foreach(path IN LISTS paths)
    if(EXISTS "${path}")
      list(APPEND existing_paths "${path}")
    endif()
  endforeach()
  set_target_properties(${target} PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${existing_paths}")
endforeach()
add_executable(svf-fspta-observer observer.cpp)
target_compile_features(svf-fspta-observer PRIVATE cxx_std_17)
target_include_directories(svf-fspta-observer PRIVATE ../../include)
target_link_libraries(svf-fspta-observer PRIVATE SVF::SvfLLVM)
