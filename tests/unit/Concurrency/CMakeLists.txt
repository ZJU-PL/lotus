foreach(directory Core API MHP LockSet ValueFlow Thread OpenMP MPI CUDA
                  LinuxKernel)
  get_filename_component(group "${directory}" NAME)
  string(TOLOWER "${group}" group)
  lotus_test_group_enabled(concurrency "${group}" selected)
  if(selected)
    add_subdirectory(${directory})
  endif()
endforeach()

# Selected concurrency test sources collected from the subdirectories above are
# linked into one binary so Concurrency/CanaryParallel are linked only once.
add_lotus_collected_test_suite(concurrency_tests
  LINK_LIBS Concurrency CanaryParallel)

# Preserve the established build entry point.
add_custom_target(concurrency_core_tests
  DEPENDS concurrency_tests)
