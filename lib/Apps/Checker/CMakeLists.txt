# GVFA Vulnerability Checkers
set(GVFAChecker_SOURCES
  gvfa/NullPointerChecker.cpp
  gvfa/UseAfterFreeChecker.cpp
  gvfa/UseOfUninitializedVariableChecker.cpp
  gvfa/FreeOfNonHeapMemoryChecker.cpp
  gvfa/InvalidUseOfStackAddressChecker.cpp
)
add_library(GVFAChecker STATIC ${GVFAChecker_SOURCES})
target_link_libraries(GVFAChecker CanaryNullPointer)

# Add subdirectories
add_subdirectory(Report)
add_subdirectory(kint)
add_subdirectory(concurrency)
