add_library(TPATaint STATIC
  Analysis/PrepareModule.cpp
  Analysis/TaintAnalysis.cpp
  Analysis/TrackingTaintAnalysis.cpp
  Annotation/ExternalTaintTable.cpp
  Engine/ExternalCallAnalysis.cpp
  Engine/Initializer.cpp
  Engine/SinkViolationChecker.cpp
  Engine/TaintPropagator.cpp
  Engine/TransferFunction.cpp
  FrontEnd/DefUseModuleBuilder.cpp
  FrontEnd/ModRefModuleAnalysis.cpp
  FrontEnd/ReachingDefModuleAnalysis.cpp
  Output/TaintPrinter.cpp
  Output/WriteDotFile.cpp
  Precision/CallTracker.cpp
  Precision/PrecisionLossTracker.cpp
  Precision/ReturnTracker.cpp
  Precision/TrackerTransferFunction.cpp
  Program/DefUseModule.cpp
)
target_link_libraries(TPATaint PUBLIC FSCSPointerAnalysis PRIVATE TPAUtil TPATransforms LLVMTransformUtils)
