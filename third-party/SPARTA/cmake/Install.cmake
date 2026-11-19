# Register from the top-level Lotus directory as well as from a standalone
# SPARTA project: EXCLUDE_FROM_ALL must not suppress the opt-in install.
function(sparta_install source_dir)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/sparta")
  configure_package_config_file(
    "${source_dir}/cmake/sparta-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/sparta-config.cmake"
    INSTALL_DESTINATION "${package_dir}")
  install(TARGETS SPARTA EXPORT sparta_target)
  install(DIRECTORY "${source_dir}/include/sparta"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES "${source_dir}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/SPARTA")
  install(EXPORT sparta_target FILE sparta_target.cmake
    NAMESPACE sparta:: DESTINATION "${package_dir}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sparta-config.cmake"
    DESTINATION "${package_dir}")
  export(TARGETS SPARTA FILE "${CMAKE_CURRENT_BINARY_DIR}/sparta_target.cmake"
    NAMESPACE sparta::)
endfunction()
