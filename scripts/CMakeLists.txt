if(LOTUS_ENABLE_IKOS_API_SPEC)
  add_subdirectory(ikos)
endif()

if(LOTUS_ENABLE_CLAM)
  add_subdirectory(clam)
endif()

if(LOTUS_ENABLE_SEAHORN)
  add_subdirectory(seahorn)
endif()
