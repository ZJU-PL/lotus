# Install from scripts/, which is not excluded in selective library builds.
set(IKOS_ROOT "${PROJECT_SOURCE_DIR}/third-party/ikos-api-spec")
install(TARGETS ikos-analyzer ikos-pp ikos-import
  RUNTIME DESTINATION bin COMPONENT ikos)
install(TARGETS CanaryIKOSAR CanaryIKOSFrontend CanaryIKOSPreprocessor
  ARCHIVE DESTINATION lib COMPONENT ikos)
foreach(component core ar frontend/llvm analyzer)
  install(DIRECTORY "${IKOS_ROOT}/${component}/include/"
    DESTINATION include COMPONENT ikos FILES_MATCHING
    PATTERN "*.hpp" PATTERN "*.h")
endforeach()
