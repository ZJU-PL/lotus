add_library(VerificationTraceAbstraction STATIC
  IcfgToNwa.cpp
  TransitionFormula.cpp
  TraceChecker.cpp
  InterpolantAutomatonBuilder.cpp
  TraceAbstractionEngine.cpp)
target_include_directories(VerificationTraceAbstraction PUBLIC
  ${PROJECT_SOURCE_DIR}/include ${Z3_INCLUDE_DIR})
target_compile_features(VerificationTraceAbstraction PUBLIC cxx_std_17)
target_link_libraries(VerificationTraceAbstraction PUBLIC
  VerificationAutomata CanaryICFG LLVMCore LLVMAnalysis ${Z3_LIBRARIES})
install(TARGETS VerificationTraceAbstraction DESTINATION lib)
