# GVFA Vulnerability Checkers
set(GVFAChecker_SOURCES  
  GVFA/NullPointerChecker.cpp
  GVFA/UseAfterFreeChecker.cpp
  GVFA/UseOfUninitializedVariableChecker.cpp
  GVFA/FreeOfNonHeapMemoryChecker.cpp
  GVFA/InvalidUseOfStackAddressChecker.cpp
)
add_library(GVFAChecker STATIC ${GVFAChecker_SOURCES})
target_link_libraries(GVFAChecker CanaryNullPointer)

# Add subdirectories
add_subdirectory(Report)
add_subdirectory(KINT)
add_subdirectory(Concurrency)
