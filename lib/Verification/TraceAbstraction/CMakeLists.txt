add_library(VerificationTraceAbstraction STATIC
  IcfgToNwa.cpp
  TransitionFormula.cpp
  TraceChecker.cpp
  InterpolantAutomatonBuilder.cpp
  TraceAbstractionEngine.cpp)
target_link_libraries(VerificationTraceAbstraction PUBLIC
  VerificationAutomata CanaryICFG LLVMCore LLVMAnalysis ${Z3_LIBRARIES})
install(TARGETS VerificationTraceAbstraction DESTINATION lib)
