find_package(Threads REQUIRED)

add_library(CanaryBootstrapCore STATIC
  Engine.cpp
  IR/PointsToSet.cpp
  IR/Program.cpp
  IR/ProgramIndex.cpp
  Preprocessing/AndersenRefinement.cpp
  Preprocessing/OneLevelFlow.cpp
  Preprocessing/RelevantSlice.cpp
  Preprocessing/Steensgaard.cpp
  Solver/GuardedSummaryEngine.cpp
  Solver/GuardedSummaryState.cpp
  Solver/GuardedSummaryTransfer.cpp
  Solver/TabulationSolver.cpp
  Support/Bdd.cpp)

target_link_libraries(CanaryBootstrapCore
  PUBLIC
    Threads::Threads)

add_library(CanaryBootstrapAA STATIC
  LLVM/BootstrapAA.cpp)

target_link_libraries(CanaryBootstrapAA
  PUBLIC
    CanaryBootstrapCore
    LLVMCore
    LLVMSupport)
