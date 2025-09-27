# Checker library
add_library(Checker
  TaintConfigParser.cpp
  TaintConfigManager.cpp
)

target_link_libraries(Checker
  CanarySupport
)

# Add subdirectories
add_subdirectory(Report)
