# CLAM Analysis Wrapper for Lotus

if(ENABLE_CLAM)

  add_library(ClamAnalysisWrapper STATIC
    ClamAnalysis.cpp
  )

  target_include_directories(ClamAnalysisWrapper PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_BINARY_DIR}/include
    ${CRAB_ROOT}/include
  )

  target_link_libraries(ClamAnalysisWrapper PUBLIC
    ClamAnalysis
  )

  message(STATUS "CLAM Analysis Wrapper configured")
else()
  message(STATUS "CLAM disabled, skipping CLAM Analysis Wrapper")
endif()

