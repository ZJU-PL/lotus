add_library(GSAF STATIC
  # API
  API/Models.cpp
  API/Trace.cpp
  API/VulnerabilityRegistry.cpp
  API/VulnerabilityWrapper.cpp

  # Engine
  Engine/Checker.cpp
  Engine/Solver.cpp
  Engine/Summaries.cpp
  Engine/IntraAnalysis.cpp
  Engine/InterAnalysis.cpp

  # Built-in checkers
  Checkers/Taint.cpp
  Checkers/UseAfterFree.cpp

  # Report
  Report/ReportDecorator.cpp

  # Support
  Support/CheckerServices.cpp
  Support/GraphQueries.cpp
  Support/Options.cpp
)

file(READ ${PROJECT_SOURCE_DIR}/config/gsaf/api.json GSAF_API_DEFAULTS)
file(READ ${PROJECT_SOURCE_DIR}/config/gsaf/taint.json GSAF_TAINT_DEFAULTS)
configure_file(API/DefaultModels.h.in DefaultModels.h @ONLY)
target_include_directories(GSAF PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(GSAF PUBLIC CanaryGuardedValueFlow CanarySMT Annotation
  CanaryParallel CanaryPlatform CanaryCFG CheckerFramework CanaryLLVMUtils CanaryMemoryAnalysis)
